#include "pm/audio/filter/models.hpp"

#include "pm/audio/path.hpp"

#include <cstdlib>
#include <mutex>
#include <sstream>

namespace pm::audio::filter {
namespace {

std::mutex& roots_mutex()
{
    static std::mutex mu;
    return mu;
}

std::vector<std::filesystem::path>& extra_roots()
{
    static std::vector<std::filesystem::path> roots;
    return roots;
}

bool is_file(const std::filesystem::path& path)
{
    std::error_code ec;
    return !path.empty() && std::filesystem::is_regular_file(path, ec) && !ec;
}

bool unpacked_deepfilter(const std::filesystem::path& dir)
{
    if (dir.empty())
        return false;
    std::error_code ec;
    if (!std::filesystem::is_directory(dir, ec) || ec)
        return false;
    const std::filesystem::path nested = dir / "tmp" / "export";
    const std::filesystem::path* roots[] = {&dir, &nested};
    for (const std::filesystem::path* root : roots) {
        if (is_file(*root / "enc.onnx") && is_file(*root / "erb_dec.onnx")
            && is_file(*root / "df_dec.onnx") && is_file(*root / "config.ini"))
            return true;
    }
    return false;
}

std::vector<std::filesystem::path> collect_model_roots()
{
    std::vector<std::filesystem::path> roots;
    {
        std::lock_guard<std::mutex> lock(roots_mutex());
        roots = extra_roots();
    }
    if (const char* env = std::getenv("MODELS_DIR")) {
        if (*env)
            roots.emplace_back(env);
    }
    const std::filesystem::path exe = exe_directory();
    if (!exe.empty()) {
        roots.push_back(exe / "shared" / "models");
        roots.push_back(exe / "models");
        roots.push_back(exe / ".." / "models");
    }
#if defined(PM_AUDIO_TANIT_ROOT)
    const std::filesystem::path tanit{PM_AUDIO_TANIT_ROOT};
    roots.push_back(tanit / "models");
    roots.push_back(tanit / "packages" / "DeepFilterNet" / "models");
    roots.push_back(tanit / "packages" / "gtcrn" / "models");
#endif
    return roots;
}

std::string joined_probes(const ModelProbe& probe)
{
    if (probe.probed.empty())
        return " (none)";
    std::ostringstream os;
    for (const std::string& path : probe.probed)
        os << "\n  - " << path;
    return os.str();
}

} // namespace

RuntimeInfo runtime_info()
{
    RuntimeInfo info;
#if defined(PM_AUDIO_HAS_ONNX) && PM_AUDIO_HAS_ONNX
    info.onnx = true;
#endif
#if defined(PM_AUDIO_HAS_WHISPER) && PM_AUDIO_HAS_WHISPER
    info.whisper = true;
#endif
#if defined(PM_AUDIO_ONNX_ROOT)
    info.onnx_root = PM_AUDIO_ONNX_ROOT;
#endif
#if defined(PM_AUDIO_WHISPER_ROOT)
    info.whisper_root = PM_AUDIO_WHISPER_ROOT;
#endif
#if defined(PM_AUDIO_TANIT_ROOT)
    info.tanit_root = PM_AUDIO_TANIT_ROOT;
#endif
    return info;
}

OnnxGate& onnx_gate()
{
    static OnnxGate gate = nullptr;
    return gate;
}

void set_onnx_gate(OnnxGate gate)
{
    std::lock_guard<std::mutex> lock(roots_mutex());
    onnx_gate() = gate;
}

bool onnx_allowed(std::string& err)
{
    OnnxGate gate = nullptr;
    {
        std::lock_guard<std::mutex> lock(roots_mutex());
        gate = onnx_gate();
    }
    if (!gate)
        return true;
    return gate(err);
}

std::vector<std::filesystem::path> model_search_roots()
{
    return collect_model_roots();
}

void set_model_roots(std::vector<std::filesystem::path> roots)
{
    std::lock_guard<std::mutex> lock(roots_mutex());
    extra_roots() = std::move(roots);
}

ModelProbe probe_deepfilter_model(const std::string& hint)
{
    ModelProbe out;
    out.hint = hint;
    out.filename = "DeepFilterNet3_onnx.tar.gz";

    auto note = [&](const std::filesystem::path& path) {
        if (!path.empty())
            out.probed.push_back(path_u8(path));
    };
    auto take_file = [&](const std::filesystem::path& path) {
        note(path);
        if (!is_file(path))
            return false;
        out.path = path_u8(path);
        return true;
    };
    auto take_dir = [&](const std::filesystem::path& path) {
        note(path);
        if (!unpacked_deepfilter(path))
            return false;
        out.path = path_u8(path);
        return true;
    };

    if (!hint.empty()) {
        const std::filesystem::path hinted(hint);
        if (is_file(hinted)) {
            out.path = hint;
            return out;
        }
        if (take_dir(hinted))
            return out;
        out.probed.push_back(hint);
    }

    const std::filesystem::path hint_name = hint.empty() ? std::filesystem::path{} : std::filesystem::path(hint).filename();
    const bool hint_is_leaf = !hint.empty() && hint_name == std::filesystem::path(hint);
    for (const auto& root : collect_model_roots()) {
        if (hint_is_leaf && take_file(root / hint_name))
            return out;
        if (take_dir(root / "deepfilter"))
            return out;
        if (take_file(root / "deepfilter" / out.filename))
            return out;
        if (take_file(root / out.filename))
            return out;
        if (take_dir(root / "DeepFilterNet3"))
            return out;
        if (take_file(root / "DeepFilterNet3" / out.filename))
            return out;
    }
    return out;
}

std::string deepfilter_model_not_found_message(const ModelProbe& probe)
{
    std::ostringstream os;
    os << "DeepFilterNet model not found (expected unpacked deepfilter/ with enc.onnx or "
       << probe.filename << ").";
    if (!probe.hint.empty())
        os << " hint: \"" << probe.hint << "\".";
    os << " Probed paths:" << joined_probes(probe);
    return os.str();
}

ModelProbe probe_gtcrn_model(const std::string& hint)
{
    ModelProbe out;
    out.hint = hint;
    out.filename = "gtcrn_simple.onnx";
    auto note = [&](const std::filesystem::path& path) {
        if (!path.empty())
            out.probed.push_back(path_u8(path));
    };
    auto take = [&](const std::filesystem::path& path) {
        note(path);
        if (!is_file(path))
            return false;
        out.path = path_u8(path);
        return true;
    };
    if (!hint.empty() && is_file(std::filesystem::path(hint))) {
        out.path = hint;
        return out;
    }
    if (!hint.empty())
        out.probed.push_back(hint);
    const std::filesystem::path hint_name = hint.empty() ? std::filesystem::path{} : std::filesystem::path(hint).filename();
    const bool hint_is_leaf = !hint.empty() && hint_name == std::filesystem::path(hint);
    for (const auto& root : collect_model_roots()) {
        if (hint_is_leaf && take(root / hint_name))
            return out;
        if (take(root / out.filename))
            return out;
        if (take(root / "gtcrn" / out.filename))
            return out;
    }
    return out;
}

std::string gtcrn_model_not_found_message(const ModelProbe& probe)
{
    std::ostringstream os;
    os << "GTCRN model not found (expected " << probe.filename << ").";
    if (!probe.hint.empty())
        os << " hint: \"" << probe.hint << "\".";
    os << " Probed paths:" << joined_probes(probe);
    return os.str();
}

} // namespace pm::audio::filter
