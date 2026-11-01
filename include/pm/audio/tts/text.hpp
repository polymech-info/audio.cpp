#pragma once

#include <filesystem>
#include <string>
#include <string_view>

namespace pm::tts {

struct TtsTextInputResult {
    std::string text;
    bool from_file = false;
    std::string source_path;
    std::string preparser_id;
};

// Host path gate. Return false and set err to reject a file path.
using TtsPathCheck = bool (*)(const std::string& path, std::string& err);

// 1 = text, 0 = unknown (NUL sniff), -1 = reject.
using TtsBinaryProbe = int (*)(const std::filesystem::path& path, std::string& err);

// Applied to markdown after front matter is removed. Host installs the
// common text sanitizers. Unset keeps the body as read.
using TtsSanitizeFn = std::string (*)(std::string_view text);

void set_tts_path_check(TtsPathCheck fn);
void set_tts_binary_probe(TtsBinaryProbe fn);
void set_tts_sanitizer(TtsSanitizeFn fn);

bool resolve_tts_text(const std::string& text_or_path,
                      TtsTextInputResult& out,
                      std::string& err);

struct TtsTextPreparseContext {
    std::filesystem::path source_path;
};

bool apply_tts_text_preprocessors(const TtsTextPreparseContext& ctx,
                                 std::string& text,
                                 std::string& applied_id,
                                 std::string& err);

bool tts_md_matches(const std::filesystem::path& path, std::string_view content);

bool tts_preparse_markdown(const std::filesystem::path& path,
                           std::string_view content,
                           std::string& out,
                           std::string& err);

} // namespace pm::tts
