#include "pm/audio/filter/deepfilter.hpp"


#include "pm/audio/dynlib.hpp"
#include "pm/audio/filter/models.hpp"
#include "pm/audio/path.hpp"
#include "pm/audio/text.hpp"
#include "pm/audio/filter/deep_filter_capi.h"
#include "pm/audio/log.hpp"

#include <algorithm>
#include <cstring>
#include <memory>
#include <mutex>
#include <vector>

#if defined(_WIN32)
#  ifndef WIN32_LEAN_AND_MEAN
#    define WIN32_LEAN_AND_MEAN
#  endif
#  include <windows.h>
#endif

namespace pm::audio::filter::dfn {
namespace {

namespace fs = std::filesystem;

#if defined(_WIN32)
constexpr const char* k_dll_names[] = { "deepfilter.dll", "df.dll" };
#elif defined(__APPLE__)
constexpr const char* k_dll_names[] = { "libdeepfilter.dylib", "libdf.dylib" };
#else
constexpr const char* k_dll_names[] = { "libdeepfilter.so", "libdf.so" };
#endif

struct DfApi {
    using Create = DFState* (*)(const char*, float, const char*);
    using FrameLength = size_t (*)(DFState*);
    using SetAtten = void (*)(DFState*, float);
    using SetPf = void (*)(DFState*, float);
    using Process = float (*)(DFState*, float*, float*);
    using Free = void (*)(DFState*);

    void*        module = nullptr;
    Create       create = nullptr;
    FrameLength  frame_length = nullptr;
    SetAtten     set_atten = nullptr;
    SetPf        set_pf = nullptr;
    Process      process = nullptr;
    Free         free = nullptr;
    std::string  loaded_path;
    std::string  error;
};

DfApi& api() noexcept
{
    static DfApi state;
    return state;
}

std::mutex& api_mutex()
{
    static std::mutex m;
    return m;
}

std::vector<std::filesystem::path> dll_search_dirs()
{
    std::vector<std::filesystem::path> dirs;
    const auto exe_dir = pm::audio::exe_directory();
    const auto bin_dir = pm::audio::exe_directory();
    if (!exe_dir.empty()) {
        dirs.push_back(exe_dir);
        dirs.push_back(exe_dir / "bin");
        dirs.push_back(exe_dir / ".." / "packages" / "DeepFilterNet" / "prebuilt" / "win-x64" / "bin");
        dirs.push_back(exe_dir / ".." / ".." / "packages" / "DeepFilterNet" / "prebuilt" / "win-x64" / "bin");
        dirs.push_back(exe_dir / ".." / "packages" / "DeepFilterNet" / "target" / "release");
        dirs.push_back(exe_dir / ".." / ".." / "packages" / "DeepFilterNet" / "target" / "release");
    }
#if defined(PM_AUDIO_TANIT_ROOT)
    const std::filesystem::path tanit{PM_AUDIO_TANIT_ROOT};
    dirs.push_back(tanit / "packages" / "DeepFilterNet" / "prebuilt" / "win-x64" / "bin");
    dirs.push_back(tanit / "packages" / "DeepFilterNet" / "target" / "release");
#endif
    if (!bin_dir.empty())
        dirs.push_back(bin_dir);
    return dirs;
}

bool bind_api(void* module, DfApi& state)
{
    state.create = reinterpret_cast<DfApi::Create>(pm::audio::dynlib::sym(module, "df_create"));
    state.frame_length = reinterpret_cast<DfApi::FrameLength>(
        pm::audio::dynlib::sym(module, "df_get_frame_length"));
    state.set_atten = reinterpret_cast<DfApi::SetAtten>(
        pm::audio::dynlib::sym(module, "df_set_atten_lim"));
    state.set_pf = reinterpret_cast<DfApi::SetPf>(
        pm::audio::dynlib::sym(module, "df_set_post_filter_beta"));
    state.process = reinterpret_cast<DfApi::Process>(pm::audio::dynlib::sym(module, "df_process_frame"));
    state.free = reinterpret_cast<DfApi::Free>(pm::audio::dynlib::sym(module, "df_free"));
    if (state.create && state.frame_length && state.process && state.free)
        return true;
    pm::audio::dynlib::unload(module);
    state.create = nullptr;
    state.frame_length = nullptr;
    state.set_atten = nullptr;
    state.set_pf = nullptr;
    state.process = nullptr;
    state.free = nullptr;
    return false;
}

bool ensure_loaded()
{
    DfApi& state = api();
    if (state.module)
        return true;

    std::lock_guard<std::mutex> lock(api_mutex());
    if (state.module)
        return true;

    for (const auto& dir : dll_search_dirs()) {
        for (const char* name : k_dll_names) {
            void* module = pm::audio::dynlib::load(name, pm::audio::path_to_utf8(dir));
            if (!module)
                continue;
            if (!bind_api(module, state))
                continue;
            state.module = module;
            state.loaded_path = pm::audio::path_to_utf8(dir / name);
            state.error.clear();
            pm::audio::log::info("[audio-filter] loaded " + state.loaded_path);
            return true;
        }
    }

    for (const char* name : k_dll_names) {
        if (!pm::audio::dynlib::probe(name))
            continue;
        void* module = pm::audio::dynlib::load(name);
        if (!module || !bind_api(module, state))
            continue;
        state.module = module;
        state.loaded_path = name;
        state.error.clear();
        return true;
    }

    state.error = "deepfilter.dll not found. Stage it next to the exe "
                  "(packages/DeepFilterNet/build-capi.ps1) or under prebuilt/win-x64/bin.";
    return false;
}

std::filesystem::path onnx_files_dir(const std::filesystem::path& model)
{
    std::error_code ec;
    auto has = [&](const fs::path& dir) {
        ec.clear();
        return fs::is_regular_file(dir / "enc.onnx", ec)
            && fs::is_regular_file(dir / "erb_dec.onnx", ec)
            && fs::is_regular_file(dir / "df_dec.onnx", ec)
            && fs::is_regular_file(dir / "config.ini", ec);
    };
    if (has(model))
        return model;
    const fs::path nested = model / "tmp" / "export";
    if (has(nested))
        return nested;
    return {};
}

#if defined(_WIN32)
bool run_tar_czf(const fs::path& src_dir, const fs::path& dest, std::string& err)
{
    wchar_t windir[MAX_PATH]{};
    if (GetWindowsDirectoryW(windir, MAX_PATH) == 0) {
        err = "GetWindowsDirectoryW failed";
        return false;
    }
    const fs::path tar = std::filesystem::path(windir) / L"System32" / L"tar.exe";
    if (!fs::is_regular_file(tar)) {
        err = "System32 tar.exe not found";
        return false;
    }
    std::wstring cmd = L"\"" + tar.wstring() + L"\" -C \"" + src_dir.wstring()
        + L"\" -czf \"" + dest.wstring()
        + L"\" enc.onnx erb_dec.onnx df_dec.onnx config.ini";
    std::vector<wchar_t> buf(cmd.begin(), cmd.end());
    buf.push_back(L'\0');
    STARTUPINFOW si{};
    PROCESS_INFORMATION pi{};
    si.cb = sizeof(si);
    si.dwFlags = STARTF_USESHOWWINDOW;
    si.wShowWindow = SW_HIDE;
    if (!CreateProcessW(nullptr, buf.data(), nullptr, nullptr, FALSE,
                        CREATE_NO_WINDOW, nullptr, src_dir.c_str(), &si, &pi)) {
        err = "CreateProcess tar.exe failed";
        return false;
    }
    WaitForSingleObject(pi.hProcess, INFINITE);
    DWORD code = 1;
    GetExitCodeProcess(pi.hProcess, &code);
    CloseHandle(pi.hThread);
    CloseHandle(pi.hProcess);
    if (code != 0) {
        err = "tar -czf of DeepFilterNet ONNX failed";
        return false;
    }
    return fs::is_regular_file(dest);
}
#endif

// libDF df_create only reads a gzip tar. Store packages ship the extracted
// ONNX directory (WACK forbids archives); wrap it for the current DLL.
std::filesystem::path model_for_df_create(const std::filesystem::path& model, std::string& err)
{
    std::error_code ec;
    if (fs::is_regular_file(model, ec) && !ec)
        return model;
    const fs::path onnx_dir = onnx_files_dir(model);
    if (onnx_dir.empty()) {
        err = "DeepFilterNet path is not a tar.gz or unpacked ONNX directory";
        return {};
    }
#if defined(_WIN32)
    const fs::path dest = fs::temp_directory_path(ec) / "tanit-deepfilter-onnx.tar.gz";
    if (ec) {
        err = "temp_directory_path failed";
        return {};
    }
    std::error_code te;
    const auto onnx_mtime = fs::last_write_time(onnx_dir / "enc.onnx", te);
    if (!te && fs::is_regular_file(dest, te)) {
        const auto dest_mtime = fs::last_write_time(dest, te);
        if (!te && dest_mtime >= onnx_mtime)
            return dest;
    }
    if (!run_tar_czf(onnx_dir, dest, err))
        return {};
    return dest;
#else
    err = "DeepFilterNet unpacked directory is Windows-only; pass a tar.gz";
    return {};
#endif
}

bool enhance_capi(std::vector<float>& pcm, const Options& opt, const std::filesystem::path& model, Result& r)
{
    if (!ensure_loaded()) {
        r.error = api().error;
        return false;
    }
    std::string wrap_err;
    const fs::path libdf_model = model_for_df_create(model, wrap_err);
    if (libdf_model.empty()) {
        r.error = wrap_err;
        return false;
    }
    const DfApi& fn = api();
    const std::string model_utf8 = pm::audio::path_to_utf8(libdf_model);
    DFState* st = fn.create(model_utf8.c_str(), opt.atten_lim_db, nullptr);
    if (!st) {
        r.error = "df_create failed";
        return false;
    }
    const size_t hop = fn.frame_length(st);
    if (hop == 0) {
        fn.free(st);
        r.error = "df_get_frame_length returned 0";
        return false;
    }
    if (opt.post_filter && fn.set_pf)
        fn.set_pf(st, 0.02f);

    const size_t pad = (hop - (pcm.size() % hop)) % hop;
    if (opt.compensate_delay)
        pcm.insert(pcm.begin(), hop, 0.0f);
    if (pad)
        pcm.insert(pcm.end(), pad, 0.0f);

    std::vector<float> out(pcm.size(), 0.0f);
    std::vector<float> in_hop(hop), out_hop(hop);
    for (size_t i = 0; i + hop <= pcm.size(); i += hop) {
        std::copy(pcm.begin() + static_cast<std::ptrdiff_t>(i),
                  pcm.begin() + static_cast<std::ptrdiff_t>(i + hop),
                  in_hop.begin());
        fn.process(st, in_hop.data(), out_hop.data());
        std::copy(out_hop.begin(), out_hop.end(),
                  out.begin() + static_cast<std::ptrdiff_t>(i));
    }
    fn.free(st);

    if (opt.compensate_delay && out.size() > hop)
        out.erase(out.begin(), out.begin() + static_cast<std::ptrdiff_t>(hop));
    if (pad && out.size() > pad)
        out.resize(out.size() - pad);
    pcm.swap(out);
    r.backend = "libdf";
    r.model = model_utf8;
    r.ok = true;
    return true;
}

} // namespace

std::filesystem::path discover_model(const std::string& explicit_path)
{
    const auto probe = probe_deepfilter_model(explicit_path);
    return probe.path.empty() ? std::filesystem::path{} : pm::audio::path_from_utf8(probe.path);
}

bool available()
{
    return ensure_loaded();
}

std::string backend_name()
{
    return ensure_loaded() ? std::string{"libdf"} : std::string{};
}

Result enhance_48k_mono(std::vector<float>& pcm,
                        const Options& opt,
                        const std::filesystem::path& /*src_for_temp*/)
{
    Result r;
    r.kind = "deepfilter";
    if (!ensure_loaded()) {
        r.error = api().error;
        return r;
    }
    const auto probe = probe_deepfilter_model(opt.model);
    if (probe.path.empty()) {
        r.error = deepfilter_model_not_found_message(probe);
        return r;
    }
    if (enhance_capi(pcm, opt, probe.path, r))
        return r;
    return r;
}

struct StreamSession::Impl {
    DFState* st = nullptr;
    size_t hop = 0;
    bool compensate = true;
    bool skip_first = false;
    uint16_t channels = 0;
    std::vector<float> pending;
    std::vector<float> in_hop;
    std::vector<float> out_hop;

    void emit_hop(std::vector<int16_t>& out)
    {
        const DfApi& fn = api();
        fn.process(st, in_hop.data(), out_hop.data());
        if (skip_first) {
            skip_first = false;
            return;
        }
        const uint16_t ch = channels == 0 ? 1 : channels;
        for (size_t i = 0; i < hop; ++i) {
            const float x = std::clamp(out_hop[i], -1.0f, 1.0f);
            const int16_t s = static_cast<int16_t>(x * 32767.0f);
            for (uint16_t c = 0; c < ch; ++c)
                out.push_back(s);
        }
    }

    void drain_hops(std::vector<int16_t>& out)
    {
        while (pending.size() >= hop) {
            std::copy(pending.begin(), pending.begin() + static_cast<std::ptrdiff_t>(hop),
                      in_hop.begin());
            pending.erase(pending.begin(), pending.begin() + static_cast<std::ptrdiff_t>(hop));
            emit_hop(out);
        }
    }
};

StreamSession::StreamSession() = default;
StreamSession::~StreamSession() { close(); }

bool StreamSession::open(const Options& opt, std::string& err)
{
    close();
    if (opt.kind == Kind::Gtcrn) {
        err = "gtcrn is reserved; live video filter is DeepFilterNet";
        return false;
    }
    if (!ensure_loaded()) {
        err = api().error;
        return false;
    }
    const auto probe = probe_deepfilter_model(opt.model);
    if (probe.path.empty()) {
        err = deepfilter_model_not_found_message(probe);
        return false;
    }
    const std::filesystem::path libdf_model = model_for_df_create(probe.path, err);
    if (libdf_model.empty())
        return false;
    const DfApi& fn = api();
    auto impl = std::make_unique<Impl>();
    impl->st = fn.create(pm::audio::path_to_utf8(libdf_model).c_str(), opt.atten_lim_db, nullptr);
    if (!impl->st) {
        err = "df_create failed";
        return false;
    }
    impl->hop = fn.frame_length(impl->st);
    if (impl->hop == 0) {
        fn.free(impl->st);
        impl->st = nullptr;
        err = "df_get_frame_length returned 0";
        return false;
    }
    if (opt.post_filter && fn.set_pf)
        fn.set_pf(impl->st, 0.02f);
    impl->compensate = opt.compensate_delay;
    impl->skip_first = opt.compensate_delay;
    impl->in_hop.assign(impl->hop, 0.0f);
    impl->out_hop.assign(impl->hop, 0.0f);
    if (impl->compensate)
        impl->pending.assign(impl->hop, 0.0f);
    impl_ = std::move(impl);
    return true;
}

void StreamSession::close()
{
    if (!impl_)
        return;
    if (impl_->st && ensure_loaded())
        api().free(impl_->st);
    impl_.reset();
}

bool StreamSession::is_open() const noexcept
{
    return impl_ && impl_->st;
}

void StreamSession::process(const int16_t* in, size_t frames, uint16_t channels,
                            std::vector<int16_t>& out)
{
    if (!is_open() || !in || frames == 0 || channels == 0)
        return;
    if (impl_->channels == 0)
        impl_->channels = channels;
    const uint16_t ch = impl_->channels;
    impl_->pending.reserve(impl_->pending.size() + frames);
    for (size_t i = 0; i < frames; ++i) {
        if (ch == 1) {
            impl_->pending.push_back(static_cast<float>(in[i]) / 32768.0f);
        } else {
            float acc = 0.0f;
            for (uint16_t c = 0; c < ch; ++c)
                acc += static_cast<float>(in[i * ch + c]);
            impl_->pending.push_back(acc / (32768.0f * static_cast<float>(ch)));
        }
    }
    impl_->drain_hops(out);
}

void StreamSession::flush(std::vector<int16_t>& out)
{
    if (!is_open() || impl_->pending.empty())
        return;
    const size_t keep = impl_->pending.size();
    const size_t pad = (impl_->hop - (keep % impl_->hop)) % impl_->hop;
    if (pad)
        impl_->pending.insert(impl_->pending.end(), pad, 0.0f);
    impl_->drain_hops(out);
    if (pad && out.size() >= pad * impl_->channels)
        out.resize(out.size() - pad * impl_->channels);
}

} // namespace pm::audio::filter::dfn

