#include "pm/audio/tts/moss_tts.hpp"
#include "pm/audio/text.hpp"
#include "pm/audio/dynlib.hpp"
#include "pm/audio/log.hpp"
#include "pm/audio/path.hpp"



#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <limits>
#include <vector>

namespace pm::tts {
namespace {
MossSearchFn& moss_search_slot()
{
    static MossSearchFn fn = nullptr;
    return fn;
}


struct moss_nano;
using MossChunkCallback = int (*)(const float*, int, int, void*);

struct MossApi {
    using Version = const char* (*)();
    using NanoLoad = moss_nano* (*)(const char*, const char*, const char*);
    using NanoFree = void (*)(moss_nano*);
    using NanoTts = float* (*)(moss_nano*, const char*, const char*, int, int*, int*);
    using NanoStream =
        int (*)(moss_nano*, const char*, const char*, int, MossChunkCallback, void*, int*);
    using Free = void (*)(void*);

    void* module = nullptr;
    Version version = nullptr;
    NanoLoad nano_load = nullptr;
    NanoFree nano_free = nullptr;
    NanoTts nano_tts = nullptr;
    NanoStream nano_stream = nullptr;
    Free free_buffer = nullptr;
    std::string loaded_path;
    std::string error;
};

MossApi& api()
{
    static MossApi value;
    return value;
}

std::mutex& api_mutex()
{
    static std::mutex value;
    return value;
}

#if defined(_WIN32)
constexpr const char* k_library_names[] = { "moss-tts.dll", "moss_tts.dll" };
#elif defined(__APPLE__)
constexpr const char* k_library_names[] = { "libmoss-tts.dylib", "libmoss_tts.dylib" };
#else
constexpr const char* k_library_names[] = { "libmoss-tts.so", "libmoss_tts.so" };
#endif

std::vector<std::filesystem::path> library_search_dirs()
{
    std::vector<std::filesystem::path> dirs;
    if (MossSearchFn fn = moss_search_slot())
        fn(dirs);
    const auto exe = pm::audio::exe_directory();
    if (!exe.empty()) {
        dirs.push_back(exe);
        dirs.push_back(exe / "bin");
    }
#if defined(PM_AUDIO_TANIT_ROOT)
    const std::filesystem::path tanit{PM_AUDIO_TANIT_ROOT};
    dirs.push_back(tanit / "packages" / "moss-tts.cpp" / "prebuilt" / "win-x64" / "bin");
    dirs.push_back(tanit / "packages" / "moss-tts.cpp" / "build" / "Release");
#endif
    return dirs;
}

bool bind_api(void* module, MossApi& out)
{
    out.version = reinterpret_cast<MossApi::Version>(
        pm::audio::dynlib::sym(module, "moss_tts_version"));
    out.nano_load = reinterpret_cast<MossApi::NanoLoad>(
        pm::audio::dynlib::sym(module, "moss_nano_load"));
    out.nano_free = reinterpret_cast<MossApi::NanoFree>(
        pm::audio::dynlib::sym(module, "moss_nano_free"));
    out.nano_tts = reinterpret_cast<MossApi::NanoTts>(
        pm::audio::dynlib::sym(module, "moss_nano_tts"));
    out.nano_stream = reinterpret_cast<MossApi::NanoStream>(
        pm::audio::dynlib::sym(module, "moss_nano_tts_stream"));
    out.free_buffer = reinterpret_cast<MossApi::Free>(
        pm::audio::dynlib::sym(module, "moss_free"));
    if (out.nano_load && out.nano_free && out.nano_tts &&
        out.nano_stream && out.free_buffer)
        return true;

    pm::audio::log::warn("[moss-tts] shared library is missing required Nano C API symbols");
    out.error = "moss-tts shared library is missing required Nano C API symbols";
    out.version = nullptr;
    out.nano_load = nullptr;
    out.nano_free = nullptr;
    out.nano_tts = nullptr;
    out.nano_stream = nullptr;
    out.free_buffer = nullptr;
    return false;
}

bool ensure_api()
{
    MossApi& state = api();
    if (state.module)
        return true;

    std::lock_guard<std::mutex> lock(api_mutex());
    if (state.module)
        return true;

    for (const auto& dir : library_search_dirs()) {
        for (const char* name : k_library_names) {
            std::error_code ec;
            if (!std::filesystem::is_regular_file(dir / name, ec) || ec)
                continue;
            void* module = pm::audio::dynlib::load(name, pm::audio::path_to_utf8(dir));
            if (!module) {
                pm::audio::log::warn("[moss-tts] probe found " +
                             pm::audio::path_to_utf8(dir / name) + " but load failed");
                continue;
            }
            if (!bind_api(module, state)) {
                pm::audio::dynlib::unload(module);
                continue;
            }
            state.module = module;
            state.loaded_path = pm::audio::path_to_utf8(dir / name);
            state.error.clear();
            const char* ver = state.version ? state.version() : "";
            pm::audio::log::info("[moss-tts] loaded " + state.loaded_path +
                         (ver && *ver ? (" version=" + std::string(ver)) : ""));
            return true;
        }
    }

    for (const char* name : k_library_names) {
        if (!pm::audio::dynlib::probe(name))
            continue;
        void* module = pm::audio::dynlib::load(name);
        if (!module || !bind_api(module, state)) {
            if (module)
                pm::audio::dynlib::unload(module);
            continue;
        }
        state.module = module;
        state.loaded_path = name;
        state.error.clear();
        const char* ver = state.version ? state.version() : "";
        pm::audio::log::info("[moss-tts] loaded " + state.loaded_path +
                     (ver && *ver ? (" version=" + std::string(ver)) : ""));
        return true;
    }

    state.error = "moss-tts.dll not found; run packages/moss-tts.cpp/build-win.ps1 "
                  "and stage the DLL under plugins/moss/ (or next to the executable)";
    pm::audio::log::warn("[moss-tts] " + state.error);
    return false;
}

template <typename T>
void write_le(std::ofstream& out, T value)
{
    out.write(reinterpret_cast<const char*>(&value), sizeof(value));
}

bool write_pcm16_wav(const std::string& path,
                     const float* samples,
                     int sample_count,
                     int sample_rate,
                     int channels)
{
    if (!samples || sample_count <= 0 || sample_rate <= 0 || channels <= 0)
        return false;

    const std::uint64_t data_size64 =
        static_cast<std::uint64_t>(sample_count) * sizeof(std::int16_t);
    if (data_size64 > std::numeric_limits<std::uint32_t>::max() - 36U)
        return false;

    std::error_code ec;
    const auto parent = pm::audio::path_from_utf8(path).parent_path();
    if (!parent.empty())
        std::filesystem::create_directories(parent, ec);
    if (ec)
        return false;

    std::ofstream out(path, std::ios::binary | std::ios::trunc);
    if (!out)
        return false;

    const auto data_size = static_cast<std::uint32_t>(data_size64);
    const auto byte_rate =
        static_cast<std::uint32_t>(sample_rate * channels * sizeof(std::int16_t));
    const auto block_align =
        static_cast<std::uint16_t>(channels * sizeof(std::int16_t));
    out.write("RIFF", 4);
    write_le(out, static_cast<std::uint32_t>(36U + data_size));
    out.write("WAVEfmt ", 8);
    write_le(out, static_cast<std::uint32_t>(16));
    write_le(out, static_cast<std::uint16_t>(1));
    write_le(out, static_cast<std::uint16_t>(channels));
    write_le(out, static_cast<std::uint32_t>(sample_rate));
    write_le(out, byte_rate);
    write_le(out, block_align);
    write_le(out, static_cast<std::uint16_t>(16));
    out.write("data", 4);
    write_le(out, data_size);

    for (int i = 0; i < sample_count; ++i) {
        const float clamped = std::max(-1.0f, std::min(1.0f, samples[i]));
        const auto pcm = static_cast<std::int16_t>(
            std::lrint(clamped * (clamped < 0.0f ? 32768.0f : 32767.0f)));
        write_le(out, pcm);
    }
    return static_cast<bool>(out);
}

struct StreamState {
    const MossPcmCallback* callback = nullptr;
    bool cancelled = false;
};

int stream_trampoline(const float* pcm, int frames, int channels, void* userdata)
{
    auto* state = static_cast<StreamState*>(userdata);
    if (!state || !state->callback || !*state->callback)
        return 1;
    if ((*state->callback)(pcm, frames, channels))
        return 0;
    state->cancelled = true;
    return 1;
}

} // namespace

void set_moss_search(MossSearchFn fn)
{
    moss_search_slot() = fn;
}

MossNanoEngine& MossNanoEngine::global()
{
    static MossNanoEngine engine;
    static const int unload_at_exit = (std::atexit([] {
        auto& value = MossNanoEngine::global();
        std::lock_guard<std::mutex> lock(value.mu_);
        if (value.handle_ && api().nano_free) {
            pm::audio::log::info("[moss-tts] atexit: unloading engine");
            api().nano_free(static_cast<moss_nano*>(value.handle_));
            value.handle_ = nullptr;
        }
    }), 0);
    (void)unload_at_exit;
    return engine;
}

MossNanoEngine::~MossNanoEngine()
{
    handle_ = nullptr;
}

int MossNanoEngine::ensure(const std::string& model,
                           const std::string& codec,
                           const std::string& tokenizer)
{
    std::lock_guard<std::mutex> lock(mu_);
    if (model.empty() || codec.empty() || tokenizer.empty()) {
        error_ = "model, codec, and tokenizer GGUF paths are required";
        pm::audio::log::warn("[moss-tts] " + error_);
        return -1;
    }
    if (handle_ && model_ == model && codec_ == codec && tokenizer_ == tokenizer) {
        pm::audio::log::trace("[moss-tts] engine already warm");
        return 0;
    }
    if (!ensure_api()) {
        error_ = api().error;
        pm::audio::log::warn("[moss-tts] " + error_);
        return -2;
    }
    if (handle_) {
        pm::audio::log::info("[moss-tts] reloading engine");
        api().nano_free(static_cast<moss_nano*>(handle_));
        handle_ = nullptr;
    }

    pm::audio::log::info("[moss-tts] moss_nano_load model=" + model +
                 " codec=" + codec + " tokenizer=" + tokenizer);
    handle_ = api().nano_load(model.c_str(), codec.c_str(), tokenizer.c_str());
    if (!handle_) {
        error_ = "moss_nano_load failed; verify the three Nano GGUF files and DLL revision";
        pm::audio::log::warn("[moss-tts] " + error_);
        return -3;
    }
    model_ = model;
    codec_ = codec;
    tokenizer_ = tokenizer;
    error_.clear();
    pm::audio::log::info("[moss-tts] engine loaded");
    return 0;
}

int MossNanoEngine::tts(const std::string& text,
                        const std::string& ref_audio,
                        const std::string& out_wav,
                        int seed)
{
    std::lock_guard<std::mutex> lock(mu_);
    if (!handle_) {
        error_ = "MOSS Nano engine is not loaded";
        pm::audio::log::warn("[moss-tts] " + error_);
        return -1;
    }

    int sample_count = 0;
    int sample_rate = 0;
    const char* ref = ref_audio.empty() ? nullptr : ref_audio.c_str();
    pm::audio::log::trace("[moss-tts] tts chars=" + std::to_string(text.size()) +
                  " ref=" + (ref ? ref_audio : std::string("(none)")) +
                  " dst=" + out_wav);
    float* pcm = api().nano_tts(static_cast<moss_nano*>(handle_),
                                text.c_str(), ref, seed,
                                &sample_count, &sample_rate);
    if (!pcm) {
        error_ = "moss_nano_tts failed";
        pm::audio::log::warn("[moss-tts] " + error_);
        return -2;
    }
    const bool ok = write_pcm16_wav(out_wav, pcm, sample_count, sample_rate, 2);
    api().free_buffer(pcm);
    if (!ok) {
        error_ = "failed to write MOSS 48 kHz stereo WAV: " + out_wav;
        pm::audio::log::warn("[moss-tts] " + error_);
        return -3;
    }
    error_.clear();
    pm::audio::log::info("[moss-tts] wrote " + out_wav +
                 " samples=" + std::to_string(sample_count) +
                 " sr=" + std::to_string(sample_rate) + " ch=2");
    return 0;
}

int MossNanoEngine::stream(const std::string& text,
                           const std::string& ref_audio,
                           const MossPcmCallback& callback,
                           int& sample_rate,
                           int seed)
{
    std::lock_guard<std::mutex> lock(mu_);
    sample_rate = 0;
    if (!handle_ || !callback) {
        error_ = handle_ ? "MOSS stream callback is empty" : "MOSS Nano engine is not loaded";
        pm::audio::log::warn("[moss-tts] " + error_);
        return -1;
    }

    StreamState state{ &callback, false };
    const char* ref = ref_audio.empty() ? nullptr : ref_audio.c_str();
    pm::audio::log::trace("[moss-tts] stream chars=" + std::to_string(text.size()) +
                  " ref=" + (ref ? ref_audio : std::string("(none)")));
    const int rc = api().nano_stream(static_cast<moss_nano*>(handle_),
                                     text.c_str(), ref, seed,
                                     stream_trampoline, &state, &sample_rate);
    if (state.cancelled) {
        error_ = "MOSS synthesis cancelled";
        pm::audio::log::info("[moss-tts] " + error_);
        return 130;
    }
    if (rc != 0) {
        error_ = "moss_nano_tts_stream failed (rc=" + std::to_string(rc) + ")";
        pm::audio::log::warn("[moss-tts] " + error_);
        return rc;
    }
    error_.clear();
    pm::audio::log::info("[moss-tts] stream complete sr=" + std::to_string(sample_rate));
    return 0;
}

void MossNanoEngine::unload()
{
    std::lock_guard<std::mutex> lock(mu_);
    if (handle_ && api().nano_free) {
        pm::audio::log::info("[moss-tts] unloading engine");
        api().nano_free(static_cast<moss_nano*>(handle_));
    }
    handle_ = nullptr;
    model_.clear();
    codec_.clear();
    tokenizer_.clear();
}

bool MossNanoEngine::is_loaded() const
{
    std::lock_guard<std::mutex> lock(mu_);
    return handle_ != nullptr;
}

std::string MossNanoEngine::last_error() const
{
    std::lock_guard<std::mutex> lock(mu_);
    return error_;
}

nlohmann::json MossNanoEngine::loaded_models_json() const
{
    std::unique_lock<std::mutex> lock(mu_, std::try_to_lock);
    if (!lock.owns_lock()) {
        return nlohmann::json::array({{
            {"key", "moss:tts"},
            {"kind", "moss-tts-nano"},
            {"name", "MOSS-TTS-Nano"},
            {"status", "busy"},
            {"provider", "moss"},
            {"stopSupported", true},
        }});
    }
    if (!handle_)
        return nlohmann::json::array();
    return nlohmann::json::array({{
        {"key", "moss:tts"},
        {"kind", "moss-tts-nano"},
        {"name", pm::audio::path_to_utf8(pm::audio::path_from_utf8(model_).filename())},
        {"path", model_},
        {"codecPath", codec_},
        {"tokenizerPath", tokenizer_},
        {"status", "loaded"},
        {"provider", "moss"},
        {"stopSupported", true},
    }});
}

bool MossNanoEngine::unload_loaded_model(std::string_view key, std::string& err)
{
    if (!key.empty() && key != "moss:tts") {
        err = "not a MOSS model key";
        return false;
    }
    std::unique_lock<std::mutex> lock(mu_, std::try_to_lock);
    if (!lock.owns_lock()) {
        err = "MOSS engine is busy";
        return false;
    }
    if (!handle_) {
        err = "no MOSS engine loaded";
        return false;
    }
    pm::audio::log::info("[moss-tts] unload_loaded_model moss:tts");
    api().nano_free(static_cast<moss_nano*>(handle_));
    handle_ = nullptr;
    model_.clear();
    codec_.clear();
    tokenizer_.clear();
    return true;
}

} // namespace pm::tts

