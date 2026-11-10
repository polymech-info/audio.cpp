#include "reader_optimize_default.hpp"

#include "fs/path_sanitizer.hpp"
#include "llm/change_store.hpp"
#include "llm/path_tool_executor.hpp"

#include <nlohmann/json.hpp>

#include <algorithm>
#include <chrono>
#include <cctype>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <sstream>
#include <vector>

namespace reader_optimize {
namespace {

namespace fs = std::filesystem;

constexpr const char* k_question =
    "Read the printed words on this page. List only what is visible. Do not add a story.";

constexpr const char* k_system =
    "You review one spoken page. Return one JSON object and nothing else, with no markdown fence. "
    "{\"revised_text\":\"...\",\"changes\":[{\"kind\":\"visual_claim\",\"before\":\"...\","
    "\"after\":\"...\",\"requires_vision\":true,\"question\":\"Does the picture show this?\"}],"
    "\"printed_text_preserved\":true,\"needs_review\":false}. "
    "revised_text is only the words to speak, with no heading. "
    "Any sentence that says what a picture shows is a visual_claim, including one you copy unchanged. "
    "Set requires_vision true and ask one yes/no question about the visible action or object. "
    "printed_text_preserved is only your assertion.";

const Preset k_presets[] = {
    {"reader-review", 1,
     "Check this spoken page against the harvested text. "
     "Correct omissions and contradictions only. Do not restyle it. "
     "Neighbor summaries are context, not evidence. "
     "Following pages identify people. Do not narrate their events. "
     "Put the spoken words in revised_text and flag every changed picture claim.",
     2, 2, 5, "on_demand"},
    {"reader-accessible-child", 1,
     "Review this page of an illustrated children's book for spoken delivery. "
     "Preserve story events and printed dialogue. "
     "When the page is a picture, say what happens and where, then who does it. "
     "A listener who cannot see the page should be able to picture the action. "
     "Do not speak the page number printed in the corner. "
     "If the page has several pictures, keep the count and describe each one. "
     "When the page is only type, the paper color may be omitted. "
     "Keep the language simple. Do not invent events. "
     "Keep every printed line, including one that says Read along. "
     "Following pages identify people. Do not narrate their events. "
     "Neighbor text is context, not evidence. "
     "Put the spoken words in revised_text and flag every picture claim, including one you keep.",
     2, 2, 5, "on_demand"},
    {"reader-vision-verify", 1,
     "A fresh look at this page's picture is included when one was made. "
     "Correct the narration where it misses printed words or describes something that look does not support. "
     "Keep what the look supports. Do not invent. "
     "Following pages identify people. Do not narrate their events. "
     "Put the spoken words in revised_text and flag every changed picture claim.",
     2, 2, 5, "always"},
    {"reader-narrative", 1,
     "Improve the transitions and spoken readability of this page. "
     "Preserve events and printed dialogue. Do not add events from neighboring pages. "
     "Following pages identify people. Do not narrate their events. "
     "Put the spoken words in revised_text and flag every changed picture claim.",
     2, 2, 5, "on_demand"},
    {"reader-faithful", 1,
     "Correct only a demonstrable extraction error on this page. "
     "If the narration already matches the harvested text, return it unchanged. "
     "Do not restyle it and do not add description. "
     "Following pages identify people. Do not narrate their events. "
     "Put the spoken words in revised_text and flag every changed picture claim.",
     2, 2, 5, "on_demand"},
};

bool same_id(std::string_view a, std::string_view b) {
    if (a.size() != b.size())
        return false;
    for (std::size_t i = 0; i < a.size(); ++i) {
        const auto ca = static_cast<unsigned char>(a[i]);
        const auto cb = static_cast<unsigned char>(b[i]);
        if (std::tolower(ca) != std::tolower(cb))
            return false;
    }
    return true;
}

std::string trim(std::string s) {
    const auto b = s.find_first_not_of(" \t\r\n");
    if (b == std::string::npos)
        return {};
    const auto e = s.find_last_not_of(" \t\r\n");
    return s.substr(b, e - b + 1);
}

std::string read_text(const fs::path& path, std::string& err) {
    std::ifstream in(path, std::ios::binary);
    if (!in) {
        err = "cannot read " + media::path::path_to_utf8(path);
        return {};
    }
    std::ostringstream ss;
    ss << in.rdbuf();
    return ss.str();
}

std::string excerpt(const std::string& text, std::size_t cap) {
    if (text.size() <= cap)
        return text;
    return text.substr(0, cap) + "...";
}

std::string fold_spoken(std::string s) {
    std::string out;
    out.reserve(s.size());
    auto space = [&]() {
        if (!out.empty() && out.back() != ' ')
            out.push_back(' ');
    };
    for (unsigned char c : s) {
        if (c == '+') { space(); out += "plus"; space(); continue; }
        if (c == '=') { space(); out += "equals"; space(); continue; }
        if (c == '?') { space(); out += "question mark"; space(); continue; }
        if (c == '-') { space(); out += "minus"; space(); continue; }
        if (std::isalnum(c))
            out.push_back(static_cast<char>(std::tolower(c)));
        else
            space();
    }
    return trim(out);
}

bool folded_has(const std::string& hay, const std::string& needle) {
    const std::string n = fold_spoken(needle);
    if (n.size() < 8)
        return true;
    return fold_spoken(hay).find(n) != std::string::npos;
}

void push_span(std::vector<std::string>& spans, const std::string& span) {
    const std::string t = trim(span);
    if (fold_spoken(t).size() < 8)
        return;
    for (const std::string& have : spans) {
        if (fold_spoken(have) == fold_spoken(t))
            return;
    }
    spans.push_back(t);
}

std::vector<std::string> required_spans(const std::string& harvest) {
    std::vector<std::string> spans;
    auto quotes = [&](const std::string& open, const std::string& close) {
        for (std::size_t at = 0; at < harvest.size();) {
            const std::size_t a = harvest.find(open, at);
            if (a == std::string::npos)
                return;
            const std::size_t b = harvest.find(close, a + open.size());
            if (b == std::string::npos)
                return;
            push_span(spans, harvest.substr(a + open.size(), b - (a + open.size())));
            at = b + close.size();
        }
    };
    quotes("\"", "\"");
    quotes("\xE2\x80\x9C", "\xE2\x80\x9D");
    if (fold_spoken(harvest).find("read along") != std::string::npos)
        push_span(spans, "Read along");
    std::size_t line = 0;
    while (line < harvest.size()) {
        std::size_t end = harvest.find('\n', line);
        if (end == std::string::npos)
            end = harvest.size();
        const std::string row = trim(harvest.substr(line, end - line));
        const std::size_t dot = row.find(". ");
        if (dot != std::string::npos && dot >= 3 && dot <= 40) {
            int words = 1;
            for (char c : row.substr(0, dot)) {
                if (c == ' ')
                    ++words;
            }
            if (words <= 4)
                push_span(spans, row.substr(0, dot));
        }
        line = end + 1;
    }
    return spans;
}

struct Change {
    std::string kind;
    std::string before;
    std::string after;
    std::string question;
    bool requires_vision = false;
};

struct Proposal {
    bool ok = false;
    std::string revised;
    std::vector<Change> changes;
    bool printed_asserted = false;
};

Proposal parse_proposal(const std::string& raw) {
    Proposal out;
    const std::size_t a = raw.find('{');
    const std::size_t b = raw.rfind('}');
    if (a == std::string::npos || b == std::string::npos || b < a)
        return out;
    const nlohmann::json j = nlohmann::json::parse(raw.substr(a, b - a + 1), nullptr, false);
    if (j.is_discarded() || !j.is_object() || !j.contains("revised_text") || !j["revised_text"].is_string())
        return out;
    out.ok = true;
    out.revised = j["revised_text"].get<std::string>();
    out.printed_asserted = j.value("printed_text_preserved", false);
    if (j.contains("changes") && j["changes"].is_array()) {
        for (const auto& item : j["changes"]) {
            if (!item.is_object())
                continue;
            Change change;
            change.kind = item.value("kind", std::string{});
            change.before = item.value("before", std::string{});
            change.after = item.value("after", std::string{});
            change.question = item.value("question", std::string{});
            change.requires_vision = item.value("requires_vision", false)
                || change.kind == "visual_claim";
            if (change.requires_vision && change.question.empty())
                change.question = "Does the picture show this: " + change.after;
            out.changes.push_back(std::move(change));
        }
    }
    return out;
}

std::string vision_verdict(const std::string& answer) {
    std::string t = trim(answer);
    for (char& c : t)
        c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    const auto mark = t.find_first_not_of(" \t\r\n*_#`\"'");
    if (mark == std::string::npos)
        return "uncertain";
    t.erase(0, mark);
    auto word = [&](const char* w) {
        const std::size_t n = std::strlen(w);
        if (t.size() < n || t.compare(0, n, w) != 0)
            return false;
        if (t.size() == n)
            return true;
        return !std::isalpha(static_cast<unsigned char>(t[n]));
    };
    if (word("no") || word("not") || word("isn't") || word("isnt")
        || t.rfind("does not", 0) == 0 || t.rfind("do not", 0) == 0
        || t.rfind("is not", 0) == 0)
        return "unsupported";
    if (word("yes"))
        return "supported";
    return "uncertain";
}

fs::path canonical_or_abs(const fs::path& path) {
    std::error_code ec;
    fs::path canon = fs::weakly_canonical(path, ec);
    if (!ec && !canon.empty())
        return canon;
    return fs::absolute(path).lexically_normal();
}

bool under(const fs::path& root, const fs::path& path) {
    const fs::path rel = canonical_or_abs(path).lexically_relative(canonical_or_abs(root));
    if (rel.empty())
        return false;
    for (const fs::path& part : rel) {
        if (part == "..")
            return false;
    }
    return rel != ".";
}

bool allowed_output(const fs::path& job, const fs::path& path) {
    const fs::path job_c = canonical_or_abs(job);
    const fs::path path_c = canonical_or_abs(path);
    if (path_c.filename() == "spoken.md" && path_c.parent_path() == job_c)
        return true;
    return under(job_c / "revisions", path_c);
}

bool write_derived(const fs::path& job, const fs::path& path, const std::string& content, std::string& err) {
    std::error_code ec;
    const fs::path abs = fs::absolute(path, ec);
    const fs::path job_abs = fs::absolute(job, ec);
    if (!allowed_output(job_abs, abs)) {
        err = "refusing to write " + media::path::path_to_utf8(abs);
        return false;
    }
    // write_file joins a relative path to the fallback work folder, not the process cwd.
    const auto wrote = media::llm::path::execute("write_file", {
        {"path", media::path::path_to_utf8(abs)},
        {"content", content},
    });
    if (!wrote.ok) {
        err = wrote.error.empty() ? "write_file failed" : wrote.error;
        return false;
    }
    if (!wrote.per_file.empty() && !wrote.per_file.front().ok) {
        err = wrote.per_file.front().error.empty() ? "write_file failed" : wrote.per_file.front().error;
        return false;
    }
    return true;
}

int next_run(const fs::path& preset_dir) {
    int n = 0;
    std::error_code ec;
    if (!fs::is_directory(preset_dir, ec))
        return 1;
    for (const auto& entry : fs::directory_iterator(preset_dir, ec)) {
        const std::string name = entry.path().filename().string();
        if (name.rfind("run-", 0) != 0)
            continue;
        try {
            n = std::max(n, std::stoi(name.substr(4)));
        } catch (...) {
        }
    }
    return n + 1;
}

std::string run_name(int n) {
    std::ostringstream ss;
    ss << "run-" << std::setw(3) << std::setfill('0') << n;
    return ss.str();
}

} // namespace

const Preset& default_preset() { return k_presets[1]; }

const Preset* find(std::string_view id) {
    if (id.empty())
        return &default_preset();
    for (const Preset& preset : k_presets) {
        if (same_id(id, preset.id))
            return &preset;
    }
    return nullptr;
}

std::string ids() {
    std::string out;
    for (const Preset& preset : k_presets) {
        if (!out.empty())
            out += ", ";
        out += preset.id;
    }
    return out;
}

bool run(const pm::tts::reader::Store& store, const Preset& preset,
         const RewriteFn& rewrite, const VisionFn& vision,
         RunResult& out, std::string& err) {
    if (store.dir.empty() || store.page_count < 1) {
        err = "no pages";
        return false;
    }
    if (!rewrite) {
        err = "no rewrite";
        return false;
    }
    const fs::path job(store.dir);
    const fs::path preset_dir = job / "revisions" / preset.id;
    const int run_n = next_run(preset_dir);
    const fs::path run_dir = preset_dir / run_name(run_n);
    std::error_code ec;
    fs::create_directories(run_dir, ec);
    if (ec) {
        err = ec.message();
        return false;
    }

    struct Page {
        int index = 0;
        int source_page = 0;
        std::string harvested;
        std::string spoken;
        std::string image;
        std::string revised;
        std::string candidate;
        std::string status;
        std::string missing;
        std::string vision_question;
        std::string vision_answer;
        std::string vision_verdict;
        bool vision_requested = false;
        std::string vision_error;
        bool commit = false;
    };
    std::vector<Page> pages;
    pages.reserve(static_cast<std::size_t>(store.page_count));
    for (int i = 1; i <= store.page_count; ++i) {
        Page page;
        page.index = i;
        if (i - 1 < static_cast<int>(store.source_pages.size()))
            page.source_page = store.source_pages[static_cast<std::size_t>(i - 1)];
        pm::tts::reader::Store one = store;
        one.page = i;
        one.speak_version = pm::tts::reader::latest_page_version(one);
        const fs::path harvested(pm::tts::reader::page_file(one));
        page.harvested = read_text(harvested, err);
        if (!err.empty())
            return false;
        const fs::path spoken(pm::tts::reader::speak_file(one));
        if (spoken == harvested)
            page.spoken = page.harvested;
        else {
            page.spoken = read_text(spoken, err);
            if (!err.empty())
                return false;
        }
        const fs::path image = job / ("page-" + std::to_string(i) + ".png");
        if (fs::is_regular_file(image, ec))
            page.image = media::path::path_to_utf8(image);
        pages.push_back(std::move(page));
    }

    std::string previous_spoken;
    const fs::path prior = job / "spoken.md";
    if (fs::is_regular_file(prior, ec))
        previous_spoken = read_text(prior, err);
    err.clear();

    const int review_n = std::min(store.page_count, preset.max_pages);
    const auto started = std::chrono::steady_clock::now();
    const bool vision_on = std::string_view(preset.vision) != "off";
    const bool vision_always = std::string_view(preset.vision) == "always";
    nlohmann::json manifest_pages = nlohmann::json::array();
    std::ostringstream diff;
    diff << "# " << preset.id << " " << run_name(run_n) << "\n\n";

    for (int i = 0; i < store.page_count; ++i) {
        Page& page = pages[static_cast<std::size_t>(i)];
        if (i >= review_n) {
            page.revised = page.spoken;
            page.status = "skipped";
        } else {
            ++out.reviewed;
            pm::tts::reader::WorkProgress progress;
            progress.phase = "optimize";
            progress.index = i + 1;
            progress.total = review_n;
            progress.done = i;
            progress.whole = review_n;
            progress.elapsed_ms = std::chrono::duration_cast<std::chrono::milliseconds>(
                std::chrono::steady_clock::now() - started).count();
            std::cout << pm::tts::reader::format_progress(progress) << "\n" << std::flush;

            std::string siblings;
            std::size_t sibling_chars = 0;
            auto add_sibling = [&](int index, const char* side) {
                if (index < 1 || index > store.page_count || sibling_chars >= 1500)
                    return;
                const std::string bit = excerpt(pages[static_cast<std::size_t>(index - 1)].spoken, 400);
                if (sibling_chars + bit.size() > 1500)
                    return;
                siblings += std::string(side) + " page " + std::to_string(index)
                    + " (identity only, do not narrate these events):\n" + bit + "\n\n";
                sibling_chars += bit.size();
            };
            for (int d = preset.previous_pages; d >= 1; --d)
                add_sibling(page.index - d, "previous");
            for (int d = 1; d <= preset.next_pages; ++d)
                add_sibling(page.index + d, "following");

            std::string prompt = preset.instructions;
            if (!store.direction.empty()) {
                prompt += "\n\nSpoken direction for this later pass. It sets the audience and the tone. ";
                prompt += "Keep every person, action, and place from the harvest.\n";
                prompt += store.direction;
                prompt += "\n";
            }
            prompt += "\n\nHarvested page:\n";
            prompt += page.harvested;
            if (!siblings.empty())
                prompt += "\n\n" + siblings;
            std::string model_err;
            const std::string raw = rewrite(page.spoken, prompt, k_system, model_err);
            if (model_err == "cancelled") {
                err = "cancelled";
                return false;
            }
            const Proposal proposal = parse_proposal(raw);
            page.candidate = trim(proposal.revised);
            bool structural = proposal.ok && !page.candidate.empty()
                && page.candidate.find('\0') == std::string::npos;
            if (structural) {
                for (const Page& other : pages) {
                    if (other.index != page.index && page.candidate == trim(other.spoken)
                        && page.candidate.size() > 40) {
                        structural = false;
                        break;
                    }
                }
            }
            std::string question;
            if (structural && vision_on) {
                for (const Change& change : proposal.changes) {
                    if (change.requires_vision && !change.question.empty()) {
                        question = change.question;
                        break;
                    }
                }
                if (question.empty() && vision_always)
                    question = k_question;
            }
            ++out.printed_checks;
            std::string missing;
            if (structural) {
                for (const std::string& span : required_spans(page.harvested)) {
                    if (!folded_has(page.candidate, span)) {
                        if (!missing.empty())
                            missing += "; ";
                        missing += span;
                    }
                }
            }
            page.missing = missing;
            const bool printed_ok = structural && missing.empty();
            std::string verdict;
            if (printed_ok && !question.empty() && !page.image.empty() && vision) {
                page.vision_requested = true;
                page.vision_question = question;
                std::string vision_err;
                page.vision_answer = vision(page.image, question, vision_err);
                ++out.vision_calls;
                ++out.visual_checks;
                if (page.vision_answer.empty()) {
                    page.vision_error = vision_err.empty() ? "vision returned no text" : vision_err;
                    page.vision_verdict = "uncertain";
                } else {
                    page.vision_verdict = vision_verdict(page.vision_answer);
                }
                verdict = page.vision_verdict;
            }
            page.revised = page.spoken;
            page.commit = false;
            if (!structural) {
                page.status = "rejected";
                ++out.rejected;
            } else if (!printed_ok) {
                page.status = "rejected";
                ++out.rejected;
            } else if (verdict == "unsupported") {
                page.status = "rejected";
                ++out.rejected;
            } else if (verdict == "uncertain") {
                page.status = "vision_uncertain";
                ++out.human_review;
            } else if (verdict == "supported") {
                page.status = "vision_supported";
                page.revised = page.candidate;
                page.commit = true;
                ++out.visually_supported;
                if (trim(page.revised) != trim(page.spoken))
                    ++out.rewritten;
            } else {
                page.status = "source_text_passed";
                page.revised = page.candidate;
                page.commit = true;
                if (trim(page.revised) != trim(page.spoken))
                    ++out.rewritten;
            }
        }

        const fs::path page_out = run_dir / ("page-" + std::to_string(page.index) + ".md");
        if (page.commit) {
            if (!write_derived(job, page_out, page.revised, err))
                return false;
        }
        nlohmann::json review = {
            {"page_id", "page-" + std::to_string(page.index)},
            {"source_page", page.source_page},
            {"source_revision", store.source_revision},
            {"preset_id", preset.id},
            {"preset_version", preset.version},
            {"vision_policy", preset.vision},
            {"vision_requested", page.vision_requested},
            {"vision_question", page.vision_question},
            {"vision_verdict", page.vision_verdict},
            {"status", page.status},
            {"missing_printed", page.missing},
            {"output_hash", "sha256:" + media::llm::change::content_sha256_hex(page.revised)},
        };
        if (!page.vision_error.empty())
            review["vision_error"] = page.vision_error;
        const fs::path review_path = run_dir / ("page-" + std::to_string(page.index) + ".review.json");
        if (!write_derived(job, review_path, review.dump(2), err))
            return false;
        manifest_pages.push_back(std::move(review));

        diff << "## page " << page.index;
        if (page.source_page > 0)
            diff << " (source " << page.source_page << ")";
        diff << "\n\nstatus: " << page.status
             << "\nvision: " << (page.vision_requested ? "yes" : "no") << "\n";
        if (!page.missing.empty())
            diff << "missing printed: " << page.missing << "\n";
        if (!page.vision_question.empty())
            diff << "question: " << page.vision_question << "\n";
        if (!page.vision_verdict.empty())
            diff << "verdict: " << page.vision_verdict << "\n";
        if (!page.vision_answer.empty())
            diff << "look: " << trim(page.vision_answer) << "\n";
        if (!page.vision_error.empty())
            diff << "vision error: " << page.vision_error << "\n";
        diff << "\n### before\n\n" << trim(page.spoken)
             << "\n\n### candidate\n\n" << (page.candidate.empty() ? "(none)" : page.candidate)
             << "\n\n### committed\n\n" << trim(page.revised) << "\n\n";
    }

    std::string assembled;
    for (const Page& page : pages) {
        const std::string text = trim(page.revised);
        if (text.empty())
            continue;
        if (!assembled.empty())
            assembled += "\n\n";
        assembled += text;
    }
    if (!assembled.empty() && assembled.back() != '\n')
        assembled.push_back('\n');
    const fs::path spoken_out = run_dir / "spoken.md";
    const fs::path diff_out = run_dir / "diff.md";
    if (!previous_spoken.empty() && !write_derived(job, run_dir / "spoken.before.md", previous_spoken, err))
        return false;
    if (!write_derived(job, spoken_out, assembled, err))
        return false;
    if (!write_derived(job, diff_out, diff.str(), err))
        return false;
    nlohmann::json manifest = {
        {"preset_id", preset.id},
        {"preset_version", preset.version},
        {"experimental", true},
        {"source_revision", store.source_revision},
        {"fallback", "fallback_original"},
        {"vision_policy", preset.vision},
        {"vision_calls", out.vision_calls},
        {"reviewed", out.reviewed},
        {"rewritten", out.rewritten},
        {"visual_checks", out.visual_checks},
        {"printed_checks", out.printed_checks},
        {"visually_supported", out.visually_supported},
        {"rejected", out.rejected},
        {"human_review", out.human_review},
        {"max_pages", preset.max_pages},
        {"pages", std::move(manifest_pages)},
    };
    if (!write_derived(job, run_dir / "manifest.json", manifest.dump(2), err))
        return false;
    if (!write_derived(job, job / "spoken.md", assembled, err))
        return false;

    out.run_dir = media::path::path_to_utf8(run_dir);
    out.spoken_path = media::path::path_to_utf8(job / "spoken.md");
    out.diff_path = media::path::path_to_utf8(diff_out);
    std::cout << "optimize " << preset.id << "\n" << out.run_dir << "\n";
    std::cout << "reviewed " << out.reviewed
              << "\nrewritten " << out.rewritten
              << "\nvisual " << out.visual_checks
              << "\nprinted " << out.printed_checks
              << "\nsupported " << out.visually_supported
              << "\nrejected " << out.rejected
              << "\nreview " << out.human_review << "\n";
    return true;
}

} // namespace reader_optimize
