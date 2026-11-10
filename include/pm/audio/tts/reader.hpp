#pragma once

// Page cache for speak. Conversion and models stay outside this library.
// A document interpreter writes source text in. This stores it under
// <root>/<path hash>/ as source.md plus one file per page.

#include <cstdint>
#include <functional>
#include <string>
#include <string_view>
#include <vector>

namespace pm::tts::reader {

enum class Preset {
    Fast,
    Ask,
    Check,
};

struct PresetFlags {
    bool tools = false;
    bool mcp = false;
    bool skills = false;
    bool planner = false;
    bool single_turn = true;
};

// fast: no tools, no MCP, no skills, no planner, one turn.
// ask and check keep that clamp. Their tool allow-list is still empty.
PresetFlags preset_flags(Preset preset);
const char* preset_name(Preset preset);

struct LlmTurn {
    Preset preset = Preset::Fast;
    std::string system;
    std::string user;
};

// Returns false and sets err when the turn did not produce text.
using LlmFn = std::function<bool(const LlmTurn& turn, std::string& text, std::string& err)>;

struct Job {
    std::string source_text;
    std::string path;
    int page = 0;
    std::string direction;
    std::string language;
    std::string interpreter = "text";
    bool llm = false;
    LlmFn llm_fn;
};

struct Output {
    std::string source_text;
    std::string intent;
    std::string speak_text;
    std::string preset = "fast";
    bool llm = false;
    // True when a direction was set and no LLM ran, so speak_text is still the source.
    bool pending = false;
    std::string error;
};

Output prepare(const Job& job);

// Writes source.txt, intent.json, speak.txt, and job.json under dir.
bool write_job(const std::string& dir, const Job& job, const Output& out, std::string& err);

// Default interpreter for a file extension. "txt" and ".txt" are the same.
// text: the file is already speakable. anydoc: office, pdf, epub, csv.
// image_understand: a picture. none: this reader does not open it.
enum class Interpreter {
    None,
    Text,
    Anydoc,
    ImageUnderstand,
};

const char* interpreter_name(Interpreter interpreter);
Interpreter interpreter_for_extension(std::string_view extension);

// Stable folder name for a canonical path, or for inline text when there is no path.
std::string document_hash(std::string_view key);
std::string store_directory(const std::string& root, std::string_view key);

struct Store {
    std::string root;
    std::string dir;
    std::string path;
    std::string hash;
    std::string interpreter;
    std::string direction;
    std::string language;
    int page = 1;
    int page_count = 0;
    std::string mtime;
    bool stopped = false;
    bool pending = false;
    bool cached = false;
    // 0 speaks page-N.md. A prompt writes page-N.vK.md and sets this to K.
    int speak_version = 0;
};

// Split on the shallowest heading that occurs more than once.
// A document with no repeated heading is one page.
std::vector<std::string> pages_from_markdown(const std::string& markdown);

bool load_store(const std::string& dir, Store& store, std::string& err);

// Writes source.md, page-N.md, job.json, and state.json. Replaces older page files.
bool write_pages(const std::string& dir, Store& store, const std::string& source, std::string& err);

bool set_page(Store& store, int page, std::string& err);
bool advance_page(Store& store, std::string& err);
bool stop_pages(Store& store, std::string& err);
bool resume_pages(Store& store, std::string& err);

std::string page_file(const Store& store);

// Highest page-N.vK.md already stored for the current page. 0 when there is none.
int latest_page_version(const Store& store);

// Writes page-N.vK.md and page-N.vK.json. The original page file stays.
// speak_version becomes K. The text is the new spoken version of that page.
bool write_page_version(Store& store, const std::string& prompt, const std::string& text, std::string& err,
                        const std::string& chat_preset = {});

// The file speak should read: the current version, or the original page.
std::string speak_file(const Store& store);

} // namespace pm::tts::reader
