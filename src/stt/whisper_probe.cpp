#include "pm/audio/stt/whisper.hpp"
#include "fs/path_sanitizer.hpp"

#include "pm/audio/filter/models.hpp"
#include "pm/audio/log.hpp"
#include "pm/audio/path.hpp"

#include <filesystem>
#include <mutex>
#include <sstream>
#include "string/string_utils.hpp"

namespace pm::stt {
namespace {

std::mutex& catalog_mutex()
{
    static std::mutex mu;
    return mu;
}

WhisperCatalogFn& catalog_fn()
{
    static WhisperCatalogFn fn = nullptr;
    return fn;
}

using media::strutil::lower_ascii;

std::string trim_ascii(std::string s)
{
    std::size_t b = 0;
    std::size_t e = s.size();
    while (b < e && static_cast<unsigned char>(s[b]) <= ' ')
        ++b;
    while (e > b && static_cast<unsigned char>(s[e - 1]) <= ' ')
        --e;
    return s.substr(b, e - b);
}

std::string model_key(const std::string& id_or_file)
{
    std::string name = lower_ascii(trim_ascii(id_or_file));
    if (const auto slash = name.find_last_of("/\\"); slash != std::string::npos)
        name = name.substr(slash + 1);
    if (name.size() >= 4 && name.compare(name.size() - 4, 4, ".bin") == 0)
        name.erase(name.size() - 4);
    if (name.rfind("ggml-", 0) == 0)
        name.erase(0, 5);
    while (!name.empty() && name.front() == '-')
        name.erase(name.begin());
    return name;
}

bool file_exists(const std::string& path)
{
    if (path.empty())
        return false;
    std::error_code ec;
    return std::filesystem::is_regular_file(media::path::path_from_utf8(path), ec) && !ec;
}

} // namespace

void set_whisper_catalog(WhisperCatalogFn fn)
{
    std::lock_guard<std::mutex> lock(catalog_mutex());
    catalog_fn() = fn;
}

std::string whisper_canonical_filename(const std::string& id_or_file)
{
    const std::string key = model_key(id_or_file);
    if (key.empty())
        return {};
    return "ggml-" + key + ".bin";
}

WhisperModelProbe whisper_probe_model_path(const std::string& model, const std::string& models_dir)
{
    WhisperModelProbe out;
    out.model_id = model.empty() ? "base.en" : model;
    out.models_dir_override = models_dir;
    out.filename = whisper_canonical_filename(out.model_id);
    if (out.filename.empty())
        out.filename = media::path::path_to_utf8(media::path::path_from_utf8(out.model_id).filename());

    pm::audio::log::trace("[whisper] probe begin model=\"" + out.model_id +
                          "\" filename=" + out.filename +
                          (models_dir.empty() ? std::string{} : " override=" + models_dir));

    if (file_exists(model)) {
        out.path = model;
        out.filename = media::path::path_to_utf8(media::path::path_from_utf8(model).filename());
        pm::audio::log::trace("[whisper] probe exact path=" + out.path);
        return out;
    }

    WhisperCatalogFn catalog = nullptr;
    {
        std::lock_guard<std::mutex> lock(catalog_mutex());
        catalog = catalog_fn();
    }
    if (catalog) {
        std::string path;
        std::string filename;
        if (catalog(out.model_id, path, filename) && file_exists(path)) {
            out.path = path;
            if (!filename.empty())
                out.filename = filename;
            pm::audio::log::trace("[whisper] probe catalog hit path=" + out.path);
            return out;
        }
    }

    auto note_dir = [&](const std::filesystem::path& dir) {
        if (dir.empty() || out.filename.empty())
            return;
        out.probed.push_back(media::path::path_to_utf8(dir / out.filename));
    };
    auto try_dir = [&](const std::filesystem::path& dir) -> bool {
        if (dir.empty() || out.filename.empty())
            return false;
        const std::string cand = media::path::path_to_utf8(dir / out.filename);
        if (file_exists(cand)) {
            out.path = cand;
            return true;
        }
        return false;
    };

    std::vector<std::filesystem::path> roots;
    if (!models_dir.empty())
        roots.emplace_back(media::path::path_from_utf8(models_dir));
    for (const auto& root : pm::audio::filter::model_search_roots())
        roots.push_back(root);

    for (const auto& root : roots) {
        if (try_dir(root)) {
            pm::audio::log::trace("[whisper] probe root hit path=" + out.path);
            return out;
        }
        note_dir(root);
    }

    pm::audio::log::trace("[whisper] probe miss model=\"" + out.model_id +
                          "\" expected=" + out.filename);
    return out;
}

std::string whisper_model_not_found_message(const WhisperModelProbe& probe)
{
    std::ostringstream os;
    os << "whisper.cpp model not found.";
    os << " model id: \"" << probe.model_id << "\"";
    os << ", expected file name: " << probe.filename << ".";
    if (!probe.models_dir_override.empty())
        os << " models_dir override: \"" << probe.models_dir_override << "\".";
    os << " Probed paths:";
    if (probe.probed.empty())
        os << " (none)";
    else {
        for (const auto& p : probe.probed)
            os << "\n  - " << p;
    }
    os << "\nInstall under ${MODELS_DIR} or the install models directory (exe/../models).";
    os << " Example: ggml-large-v3-turbo.bin (recommended) or ggml-base.en.bin"
          " from https://huggingface.co/ggerganov/whisper.cpp";
    return os.str();
}

std::string whisper_resolve_model_path(const std::string& model, const std::string& models_dir)
{
    return whisper_probe_model_path(model, models_dir).path;
}

} // namespace pm::stt
