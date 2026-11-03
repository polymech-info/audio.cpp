#include "pm/audio/tts/text.hpp"
#include "fs/path_sanitizer.hpp"

#include "pm/audio/constants.hpp"

#include <algorithm>
#include <cctype>
#include <fstream>
#include <mutex>

namespace pm::tts {
namespace {

std::mutex& hook_mutex()
{
    static std::mutex mu;
    return mu;
}

TtsPathCheck& path_check_fn()
{
    static TtsPathCheck fn = nullptr;
    return fn;
}

TtsBinaryProbe& binary_probe_fn()
{
    static TtsBinaryProbe fn = nullptr;
    return fn;
}

void trim_in_place(std::string& s)
{
    while (!s.empty() && std::isspace(static_cast<unsigned char>(s.front())))
        s.erase(0, 1);
    while (!s.empty() && std::isspace(static_cast<unsigned char>(s.back())))
        s.pop_back();
}

void strip_utf8_bom(std::string& s)
{
    if (s.size() >= 3
        && static_cast<unsigned char>(s[0]) == 0xEF
        && static_cast<unsigned char>(s[1]) == 0xBB
        && static_cast<unsigned char>(s[2]) == 0xBF)
        s.erase(0, 3);
}

bool looks_like_plaintext(std::string_view bytes)
{
    const std::size_t n = std::min(bytes.size(), std::size_t{4096});
    for (std::size_t i = 0; i < n; ++i) {
        if (bytes[i] == '\0')
            return false;
    }
    return true;
}

bool load_text_file(const std::filesystem::path& path,
                    std::string& out,
                    std::string& preparser_id,
                    std::string& err)
{
    preparser_id.clear();
    TtsPathCheck check = nullptr;
    TtsBinaryProbe probe = nullptr;
    {
        std::lock_guard<std::mutex> lock(hook_mutex());
        check = path_check_fn();
        probe = binary_probe_fn();
    }
    if (check && !check(media::path::path_to_utf8(path), err)) {
        if (err.empty())
            err = "invalid path";
        return false;
    }

    std::error_code ec;
    if (!std::filesystem::is_regular_file(path, ec) || ec) {
        err = "not a readable file: " + media::path::path_to_utf8(path);
        return false;
    }
    const auto file_size = std::filesystem::file_size(path, ec);
    if (ec) {
        err = "could not stat file: " + media::path::path_to_utf8(path);
        return false;
    }
    if (file_size > pm::audio::constants::k_tts_text_file_max_bytes) {
        err = "text file too large (" + std::to_string(file_size) + " bytes; limit "
              + std::to_string(pm::audio::constants::k_tts_text_file_max_bytes) + ")";
        return false;
    }

    if (probe) {
        const int verdict = probe(path, err);
        if (verdict < 0)
            return false;
        if (verdict > 0)
            probe = nullptr;
    }

    std::ifstream in(path, std::ios::binary);
    if (!in) {
        err = "could not open file: " + media::path::path_to_utf8(path);
        return false;
    }
    out.assign(std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>());
    strip_utf8_bom(out);

    if (probe && !looks_like_plaintext(out)) {
        err = "file appears to be binary (NUL bytes in sample): " + media::path::path_to_utf8(path);
        return false;
    }

    TtsTextPreparseContext prep_ctx;
    prep_ctx.source_path = path;
    if (!apply_tts_text_preprocessors(prep_ctx, out, preparser_id, err)) {
        err = "text preprocessing failed: " + err;
        return false;
    }
    if (out.size() > pm::audio::constants::k_tts_text_max_chars) {
        err = "text too long after load (" + std::to_string(out.size()) + " chars; limit "
              + std::to_string(pm::audio::constants::k_tts_text_max_chars) + ")";
        return false;
    }
    return true;
}

struct TtsTextPreparserEntry {
    const char* id;
    int priority;
    bool (*matches)(const TtsTextPreparseContext& ctx, std::string_view text);
    bool (*preprocess)(const TtsTextPreparseContext& ctx,
                       std::string_view text,
                       std::string& out,
                       std::string& err);
};

bool md_matches_wrapper(const TtsTextPreparseContext& ctx, std::string_view text)
{
    return tts_md_matches(ctx.source_path, text);
}

bool md_preprocess_wrapper(const TtsTextPreparseContext& ctx,
                           std::string_view text,
                           std::string& out,
                           std::string& err)
{
    return tts_preparse_markdown(ctx.source_path, text, out, err);
}

} // namespace

void set_tts_path_check(TtsPathCheck fn)
{
    std::lock_guard<std::mutex> lock(hook_mutex());
    path_check_fn() = fn;
}

void set_tts_binary_probe(TtsBinaryProbe fn)
{
    std::lock_guard<std::mutex> lock(hook_mutex());
    binary_probe_fn() = fn;
}

bool resolve_tts_text(const std::string& text_or_path,
                      TtsTextInputResult& out,
                      std::string& err)
{
    err.clear();
    out = {};

    std::string trimmed = text_or_path;
    trim_in_place(trimmed);
    if (trimmed.empty()) {
        err = "text is empty";
        return false;
    }

    const std::filesystem::path path(trimmed);
    std::error_code ec;
    if (std::filesystem::is_regular_file(path, ec) && !ec) {
        if (!load_text_file(path, out.text, out.preparser_id, err))
            return false;
        out.from_file = true;
        out.source_path = media::path::path_to_utf8(path);
        return true;
    }

    out.text = std::move(trimmed);
    TtsTextPreparseContext prep_ctx;
    if (!apply_tts_text_preprocessors(prep_ctx, out.text, out.preparser_id, err))
        return false;
    if (out.text.size() > pm::audio::constants::k_tts_text_max_chars) {
        err = "text too long (" + std::to_string(out.text.size()) + " chars; limit "
              + std::to_string(pm::audio::constants::k_tts_text_max_chars) + ")";
        return false;
    }
    return true;
}

bool apply_tts_text_preprocessors(const TtsTextPreparseContext& ctx,
                                 std::string& text,
                                 std::string& applied_id,
                                 std::string& err)
{
    applied_id.clear();
    err.clear();
    static const TtsTextPreparserEntry entries[] = {
        {"markdown", 100, md_matches_wrapper, md_preprocess_wrapper},
    };
    const TtsTextPreparserEntry* best = nullptr;
    for (const auto& entry : entries) {
        if (!best || entry.priority > best->priority)
            best = &entry;
    }
    if (!best || !best->matches || !best->preprocess)
        return true;
    if (!best->matches(ctx, text))
        return true;
    std::string parsed;
    if (!best->preprocess(ctx, text, parsed, err))
        return false;
    text = std::move(parsed);
    applied_id = best->id;
    return true;
}

} // namespace pm::tts
