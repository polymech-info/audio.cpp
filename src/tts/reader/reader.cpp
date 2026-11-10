#include "pm/audio/tts/reader.hpp"

#include <nlohmann/json.hpp>

#include <cctype>
#include <cstdint>
#include <initializer_list>
#include <filesystem>
#include <fstream>
#include <system_error>

namespace pm::tts::reader {
namespace {

namespace fs = std::filesystem;

bool write_text(const fs::path& path, const std::string& text, std::string& err) {
    std::ofstream out(path, std::ios::binary | std::ios::trunc);
    if (!out) {
        err = "cannot write " + path.generic_string();
        return false;
    }
    out << text;
    if (!text.empty() && text.back() != '\n')
        out << '\n';
    if (!out) {
        err = "cannot write " + path.generic_string();
        return false;
    }
    return true;
}

} // namespace

PresetFlags preset_flags(Preset preset) {
    PresetFlags flags;
    flags.tools = false;
    flags.mcp = false;
    flags.skills = false;
    flags.planner = false;
    flags.single_turn = true;
    (void)preset;
    return flags;
}

const char* preset_name(Preset preset) {
    switch (preset) {
    case Preset::Ask: return "ask";
    case Preset::Check: return "check";
    case Preset::Fast: return "fast";
    }
    return "fast";
}

Output prepare(const Job& job) {
    Output out;
    out.source_text = job.source_text;
    out.preset = preset_name(Preset::Fast);
    out.llm = job.llm && static_cast<bool>(job.llm_fn);
    if (job.source_text.empty()) {
        out.error = "empty source";
        return out;
    }
    if (!out.llm) {
        out.intent = "speak";
        out.speak_text = job.source_text;
        out.pending = !job.direction.empty();
        return out;
    }

    LlmTurn turn;
    turn.preset = Preset::Fast;
    turn.system = "Return JSON only: {\"intent\":\"speak|what|only|translate|clarify|fact_check\",\"language\":\"\"}.";
    turn.user = job.source_text;
    if (!job.direction.empty())
        turn.user += "\n\ndirection: " + job.direction;
    if (!job.language.empty())
        turn.user += "\nlanguage: " + job.language;
    std::string text;
    std::string err;
    if (!job.llm_fn(turn, text, err)) {
        out.intent = "speak";
        out.speak_text = job.source_text;
        out.pending = true;
        out.error = err.empty() ? "llm turn failed" : err;
        return out;
    }
    try {
        const auto parsed = nlohmann::json::parse(text);
        if (parsed.contains("intent") && parsed["intent"].is_string())
            out.intent = parsed["intent"].get<std::string>();
    } catch (...) {
        out.intent.clear();
    }
    if (out.intent.empty())
        out.intent = "speak";
    if (out.intent == "speak" && job.direction.empty()) {
        out.speak_text = job.source_text;
        return out;
    }
    LlmTurn direction;
    direction.preset = Preset::Fast;
    direction.system = "Rewrite the source into text that will be spoken. Return only that text.";
    direction.user = job.source_text;
    if (!job.direction.empty())
        direction.user += "\n\ndirection: " + job.direction;
    std::string spoken;
    if (!job.llm_fn(direction, spoken, err) || spoken.empty()) {
        out.speak_text = job.source_text;
        out.pending = true;
        out.error = err.empty() ? "direction turn failed" : err;
        return out;
    }
    out.speak_text = spoken;
    return out;
}

bool write_job(const std::string& dir, const Job& job, const Output& out, std::string& err) {
    if (!out.error.empty() && out.speak_text.empty()) {
        err = out.error;
        return false;
    }
    std::error_code ec;
    fs::create_directories(fs::path(dir), ec);
    if (ec) {
        err = ec.message();
        return false;
    }
    const fs::path root(dir);
    if (!write_text(root / "source.txt", out.source_text, err))
        return false;
    if (!write_text(root / "speak.txt", out.speak_text, err))
        return false;

    const PresetFlags flags = preset_flags(Preset::Fast);
    nlohmann::json intent = {
        {"intent", out.intent},
        {"language", job.language},
        {"pending", out.pending},
    };
    nlohmann::json job_json = {
        {"path", job.path},
        {"page", job.page},
        {"direction", job.direction},
        {"language", job.language},
        {"interpreter", job.interpreter},
        {"preset", out.preset},
        {"preset_flags", {
            {"tools", flags.tools},
            {"mcp", flags.mcp},
            {"skills", flags.skills},
            {"planner", flags.planner},
            {"single_turn", flags.single_turn},
        }},
        {"llm", out.llm},
        {"pending", out.pending},
        {"intent", out.intent},
    };
    if (!write_text(root / "intent.json", intent.dump(2), err))
        return false;
    if (!write_text(root / "job.json", job_json.dump(2), err))
        return false;
    return true;
}

const char* interpreter_name(Interpreter interpreter) {
    switch (interpreter) {
    case Interpreter::Text: return "text";
    case Interpreter::Anydoc: return "anydoc";
    case Interpreter::ImageUnderstand: return "image_understand";
    case Interpreter::None: return "none";
    }
    return "none";
}

namespace {

std::string lower_ext(std::string_view extension) {
    std::string out;
    out.reserve(extension.size());
    for (unsigned char c : extension) {
        if (c == '.')
            continue;
        out.push_back(static_cast<char>(std::tolower(c)));
    }
    return out;
}

bool one_of(const std::string& ext, std::initializer_list<const char*> names) {
    for (const char* name : names) {
        if (ext == name)
            return true;
    }
    return false;
}

int heading_level_at(const std::string& text, std::size_t pos) {
    if (pos > 0 && text[pos - 1] != '\n')
        return 0;
    int level = 0;
    while (pos + static_cast<std::size_t>(level) < text.size() && text[pos + level] == '#')
        ++level;
    if (level < 1 || level > 6)
        return 0;
    const std::size_t mark = pos + static_cast<std::size_t>(level);
    if (mark >= text.size() || text[mark] != ' ')
        return 0;
    return level;
}

bool save_store(const Store& store, std::string& err) {
    const fs::path dir(store.dir);
    nlohmann::json state = {
        {"page", store.page},
        {"page_count", store.page_count},
        {"stopped", store.stopped},
        {"speak_version", store.speak_version},
    };
    nlohmann::json job = {
        {"path", store.path},
        {"hash", store.hash},
        {"interpreter", store.interpreter},
        {"direction", store.direction},
        {"language", store.language},
        {"page", store.page},
        {"page_count", store.page_count},
        {"mtime", store.mtime},
        {"stopped", store.stopped},
        {"pending", store.pending},
        {"speak_version", store.speak_version},
    };
    if (!write_text(dir / "state.json", state.dump(2), err))
        return false;
    if (!write_text(dir / "job.json", job.dump(2), err))
        return false;
    return true;
}

} // namespace

Interpreter interpreter_for_extension(std::string_view extension) {
    const std::string ext = lower_ext(extension);
    if (ext.empty())
        return Interpreter::None;
    if (one_of(ext, {"txt", "text", "md", "markdown", "log"}))
        return Interpreter::Text;
    if (one_of(ext, {"png", "jpg", "jpeg", "webp", "gif", "bmp", "tif", "tiff", "heic"}))
        return Interpreter::ImageUnderstand;
    if (one_of(ext, {
            "pdf", "epub", "doc", "docx", "docm", "odt", "rtf",
            "ppt", "pptx", "pptm", "xls", "xlsx", "xlsm", "xlsb",
            "ods", "odp", "csv"}))
        return Interpreter::Anydoc;
    return Interpreter::None;
}

std::string document_hash(std::string_view key) {
    std::uint64_t hash = 14695981039346656037ull;
    for (unsigned char c : key) {
        hash ^= c;
        hash *= 1099511628211ull;
    }
    static const char* hex = "0123456789abcdef";
    std::string out(16, '0');
    for (int i = 15; i >= 0; --i) {
        out[static_cast<std::size_t>(i)] = hex[hash & 0x0f];
        hash >>= 4;
    }
    return out;
}

std::string store_directory(const std::string& root, std::string_view key) {
    return (fs::path(root) / document_hash(key)).generic_string();
}

std::vector<std::string> pages_from_markdown(const std::string& markdown) {
    if (markdown.empty())
        return {};
    struct Hit {
        int level = 0;
        std::size_t pos = 0;
    };
    std::vector<Hit> hits;
    for (std::size_t i = 0; i <= markdown.size();) {
        const int level = heading_level_at(markdown, i);
        if (level > 0)
            hits.push_back({level, i});
        const std::size_t nl = markdown.find('\n', i);
        if (nl == std::string::npos)
            break;
        i = nl + 1;
    }
    int split_level = 0;
    for (int level = 1; level <= 6 && split_level == 0; ++level) {
        int count = 0;
        for (const Hit& hit : hits) {
            if (hit.level == level)
                ++count;
        }
        if (count >= 2)
            split_level = level;
    }
    if (split_level == 0)
        return {markdown};

    std::vector<std::size_t> cuts;
    for (const Hit& hit : hits) {
        if (hit.level == split_level)
            cuts.push_back(hit.pos);
    }
    std::vector<std::string> pages;
    if (!cuts.empty() && cuts.front() > 0) {
        const std::string preamble = markdown.substr(0, cuts.front());
        if (!preamble.empty() && preamble.find_first_not_of(" \t\r\n") != std::string::npos)
            pages.push_back(preamble);
    }
    for (std::size_t i = 0; i < cuts.size(); ++i) {
        const std::size_t end = (i + 1 < cuts.size()) ? cuts[i + 1] : markdown.size();
        pages.push_back(markdown.substr(cuts[i], end - cuts[i]));
    }
    return pages;
}

bool load_store(const std::string& dir, Store& store, std::string& err) {
    const fs::path root(dir);
    std::ifstream in(root / "job.json", std::ios::binary);
    if (!in) {
        err = "no job";
        return false;
    }
    nlohmann::json job;
    try {
        in >> job;
    } catch (const std::exception& ex) {
        err = ex.what();
        return false;
    }
    store.dir = root.generic_string();
    store.path = job.value("path", std::string());
    store.hash = job.value("hash", std::string());
    store.interpreter = job.value("interpreter", std::string());
    store.direction = job.value("direction", std::string());
    store.language = job.value("language", std::string());
    store.page = job.value("page", 1);
    store.page_count = job.value("page_count", 0);
    store.mtime = job.value("mtime", std::string());
    store.stopped = job.value("stopped", false);
    store.pending = job.value("pending", false);
    store.speak_version = job.value("speak_version", 0);
    std::ifstream state_in(root / "state.json", std::ios::binary);
    if (state_in) {
        try {
            nlohmann::json state;
            state_in >> state;
            store.page = state.value("page", store.page);
            store.page_count = state.value("page_count", store.page_count);
            store.stopped = state.value("stopped", store.stopped);
            store.speak_version = state.value("speak_version", store.speak_version);
        } catch (...) {
        }
    }
    return true;
}

bool write_pages(const std::string& dir, Store& store, const std::string& source, std::string& err) {
    std::error_code ec;
    fs::create_directories(fs::path(dir), ec);
    if (ec) {
        err = ec.message();
        return false;
    }
    const fs::path root(dir);
    store.dir = root.generic_string();
    if (!write_text(root / "source.md", source, err))
        return false;
    const std::vector<std::string> pages = pages_from_markdown(source);
    store.page_count = static_cast<int>(pages.size());
    if (store.page < 1)
        store.page = 1;
    if (store.page_count > 0 && store.page > store.page_count)
        store.page = store.page_count;
    for (int n = 0; n < store.page_count; ++n) {
        const fs::path file = root / ("page-" + std::to_string(n + 1) + ".md");
        if (!write_text(file, pages[static_cast<std::size_t>(n)], err))
            return false;
    }
    std::error_code walk_ec;
    for (const fs::directory_entry& ent : fs::directory_iterator(root, walk_ec)) {
        if (walk_ec)
            break;
        const std::string name = ent.path().filename().string();
        if (name.rfind("page-", 0) != 0 || name.size() < 8 || name.compare(name.size() - 3, 3, ".md") != 0)
            continue;
        int index = 0;
        try {
            index = std::stoi(name.substr(5, name.size() - 8));
        } catch (...) {
            continue;
        }
        if (index > store.page_count)
            fs::remove(ent.path(), walk_ec);
    }
    store.pending = !store.direction.empty();
    store.speak_version = 0;
    return save_store(store, err);
}

int latest_page_version(const Store& store) {
    if (store.dir.empty() || store.page < 1)
        return 0;
    const std::string prefix = "page-" + std::to_string(store.page) + ".v";
    int best = 0;
    std::error_code ec;
    for (const fs::directory_entry& ent : fs::directory_iterator(fs::path(store.dir), ec)) {
        if (ec)
            break;
        const std::string name = ent.path().filename().string();
        if (name.rfind(prefix, 0) != 0 || name.size() < prefix.size() + 4)
            continue;
        if (name.compare(name.size() - 3, 3, ".md") != 0)
            continue;
        try {
            const int n = std::stoi(name.substr(prefix.size(), name.size() - prefix.size() - 3));
            if (n > best)
                best = n;
        } catch (...) {
        }
    }
    return best;
}

bool write_page_version(Store& store, const std::string& prompt, const std::string& text, std::string& err,
                        const std::string& chat_preset) {
    if (store.dir.empty() || store.page < 1) {
        err = "no page";
        return false;
    }
    if (text.empty()) {
        err = "empty version";
        return false;
    }
    const int version = latest_page_version(store) + 1;
    const fs::path root(store.dir);
    const std::string stem = "page-" + std::to_string(store.page) + ".v" + std::to_string(version);
    if (!write_text(root / (stem + ".md"), text, err))
        return false;
    nlohmann::json meta = {
        {"page", store.page},
        {"version", version},
        {"prompt", prompt},
    };
    if (!chat_preset.empty())
        meta["chat_preset"] = chat_preset;
    if (!write_text(root / (stem + ".json"), meta.dump(2), err))
        return false;
    store.speak_version = version;
    store.direction = prompt;
    store.pending = false;
    return save_store(store, err);
}

std::string speak_file(const Store& store) {
    if (store.speak_version > 0) {
        return (fs::path(store.dir) / ("page-" + std::to_string(store.page)
            + ".v" + std::to_string(store.speak_version) + ".md")).generic_string();
    }
    return page_file(store);
}

bool set_page(Store& store, int page, std::string& err) {
    if (page < 1 || page > store.page_count) {
        err = "page out of range";
        return false;
    }
    store.page = page;
    store.speak_version = latest_page_version(store);
    return save_store(store, err);
}

bool advance_page(Store& store, std::string& err) {
    if (store.stopped) {
        err = "stopped";
        return false;
    }
    if (store.page >= store.page_count) {
        err = "last page";
        return false;
    }
    store.page += 1;
    store.speak_version = latest_page_version(store);
    return save_store(store, err);
}

bool stop_pages(Store& store, std::string& err) {
    store.stopped = true;
    return save_store(store, err);
}

bool resume_pages(Store& store, std::string& err) {
    store.stopped = false;
    return save_store(store, err);
}

std::string page_file(const Store& store) {
    if (store.page_count < 1 || store.dir.empty())
        return {};
    return (fs::path(store.dir) / ("page-" + std::to_string(store.page) + ".md")).generic_string();
}

} // namespace pm::tts::reader
