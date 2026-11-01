#include "pm/audio/tts/text.hpp"

#include <algorithm>
#include <cctype>
#include <mutex>
#include <string>

#if defined(PM_AUDIO_HAS_MD4C) && PM_AUDIO_HAS_MD4C
#include <md4c.h>
#endif

namespace pm::tts {
namespace {

std::mutex& sanitizer_mutex()
{
    static std::mutex mu;
    return mu;
}

TtsSanitizeFn& sanitizer_fn()
{
    static TtsSanitizeFn fn = nullptr;
    return fn;
}

void trim_in_place(std::string& s)
{
    while (!s.empty() && std::isspace(static_cast<unsigned char>(s.front())))
        s.erase(0, 1);
    while (!s.empty() && std::isspace(static_cast<unsigned char>(s.back())))
        s.pop_back();
}

void collapse_ws_for_speech(std::string& s)
{
    std::string out;
    out.reserve(s.size());
    bool pending_space = false;
    for (unsigned char uc : s) {
        if (std::isspace(uc)) {
            pending_space = !out.empty();
        } else {
            if (pending_space) {
                out += ' ';
                pending_space = false;
            }
            out += static_cast<char>(uc);
        }
    }
    trim_in_place(out);
    s = std::move(out);
}

bool ext_is_markdown(const std::filesystem::path& path)
{
    std::string ext = path.extension().string();
    for (char& c : ext)
        c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    return ext == ".md" || ext == ".markdown" || ext == ".mdx";
}

std::string markdown_body(std::string_view sv)
{
    if (sv.size() >= 3 &&
        static_cast<unsigned char>(sv[0]) == 0xEF &&
        static_cast<unsigned char>(sv[1]) == 0xBB &&
        static_cast<unsigned char>(sv[2]) == 0xBF)
        sv = sv.substr(3);
    if (sv.size() < 4 || sv.substr(0, 3) != "---")
        return std::string(sv);
    const auto nl1 = sv.find('\n');
    if (nl1 == std::string_view::npos)
        return std::string(sv);
    const auto close = sv.find("\n---", nl1);
    if (close == std::string_view::npos)
        return std::string(sv);
    std::size_t body_start = close + 4;
    if (body_start < sv.size() && sv[body_start] == '\r')
        ++body_start;
    if (body_start < sv.size() && sv[body_start] == '\n')
        ++body_start;
    return std::string(sv.substr(body_start));
}

bool looks_like_markdown(std::string_view content)
{
    if (content.size() >= 3 && content.substr(0, 3) == "---")
        return true;
    const std::size_t scan = std::min(content.size(), std::size_t{4096});
    for (std::size_t i = 0; i < scan; ++i) {
        if (content[i] == '#' && (i == 0 || content[i - 1] == '\n'))
            return true;
    }
    return false;
}

std::string strip_md_inline(std::string_view text)
{
    std::string out;
    out.reserve(text.size());
    const int n = static_cast<int>(text.size());
    for (int i = 0; i < n;) {
        const char c = text[static_cast<std::size_t>(i)];
        if (c == '!' && i + 1 < n && text[static_cast<std::size_t>(i + 1)] == '[') {
            const auto close = text.find(']', static_cast<std::size_t>(i + 2));
            if (close != std::string_view::npos) {
                out += text.substr(static_cast<std::size_t>(i + 2), close - static_cast<std::size_t>(i + 2));
                i = static_cast<int>(close) + 1;
                if (i < n && text[static_cast<std::size_t>(i)] == '(') {
                    const auto pc = text.find(')', static_cast<std::size_t>(i));
                    i = (pc != std::string_view::npos) ? static_cast<int>(pc) + 1 : i + 1;
                }
                continue;
            }
        }
        if (c == '[') {
            const auto close = text.find(']', static_cast<std::size_t>(i + 1));
            if (close != std::string_view::npos) {
                out += text.substr(static_cast<std::size_t>(i + 1), close - static_cast<std::size_t>(i + 1));
                i = static_cast<int>(close) + 1;
                if (i < n && text[static_cast<std::size_t>(i)] == '(') {
                    const auto pc = text.find(')', static_cast<std::size_t>(i));
                    i = (pc != std::string_view::npos) ? static_cast<int>(pc) + 1 : i + 1;
                }
                continue;
            }
        }
        if ((c == '*' || c == '_') && i + 1 < n && text[static_cast<std::size_t>(i + 1)] == c) {
            i += 2;
            continue;
        }
        if (c == '*' || c == '_') {
            ++i;
            continue;
        }
        if (c == '`') {
            const auto close = text.find('`', static_cast<std::size_t>(i + 1));
            if (close != std::string_view::npos) {
                out += text.substr(static_cast<std::size_t>(i + 1), close - static_cast<std::size_t>(i + 1));
                i = static_cast<int>(close) + 1;
                continue;
            }
        }
        if (c == '>' && (i == 0 || text[static_cast<std::size_t>(i - 1)] == '\n')) {
            ++i;
            continue;
        }
        out += c;
        ++i;
    }
    return out;
}

#if defined(PM_AUDIO_HAS_MD4C) && PM_AUDIO_HAS_MD4C
struct MdTtsState {
    std::string out;
    int suppress_depth = 0;
};

bool suppressing(const MdTtsState& st) { return st.suppress_depth > 0; }

void append_text(MdTtsState& st, const char* text, MD_SIZE size)
{
    if (suppressing(st) || !text || size == 0)
        return;
    st.out.append(text, size);
}

void append_break(MdTtsState& st)
{
    if (suppressing(st) || st.out.empty())
        return;
    if (st.out.back() != ' ')
        st.out += ' ';
}

int cb_enter_block(MD_BLOCKTYPE type, void* detail, void* ud)
{
    auto& st = *static_cast<MdTtsState*>(ud);
    switch (type) {
    case MD_BLOCK_CODE:
    case MD_BLOCK_HTML:
    case MD_BLOCK_TABLE:
    case MD_BLOCK_HR:
        ++st.suppress_depth;
        break;
    default:
        break;
    }
    (void)detail;
    return 0;
}

int cb_leave_block(MD_BLOCKTYPE type, void*, void* ud)
{
    auto& st = *static_cast<MdTtsState*>(ud);
    switch (type) {
    case MD_BLOCK_CODE:
    case MD_BLOCK_HTML:
    case MD_BLOCK_TABLE:
    case MD_BLOCK_HR:
        if (st.suppress_depth > 0)
            --st.suppress_depth;
        break;
    case MD_BLOCK_H:
    case MD_BLOCK_P:
    case MD_BLOCK_LI:
    case MD_BLOCK_QUOTE:
        append_break(st);
        break;
    default:
        break;
    }
    return 0;
}

int cb_enter_span(MD_SPANTYPE type, void*, void* ud)
{
    auto& st = *static_cast<MdTtsState*>(ud);
    switch (type) {
    case MD_SPAN_CODE:
    case MD_SPAN_LATEXMATH:
    case MD_SPAN_IMG:
    case MD_SPAN_DEL:
        ++st.suppress_depth;
        break;
    default:
        break;
    }
    return 0;
}

int cb_leave_span(MD_SPANTYPE type, void*, void* ud)
{
    auto& st = *static_cast<MdTtsState*>(ud);
    switch (type) {
    case MD_SPAN_CODE:
    case MD_SPAN_LATEXMATH:
    case MD_SPAN_IMG:
    case MD_SPAN_DEL:
        if (st.suppress_depth > 0)
            --st.suppress_depth;
        break;
    default:
        break;
    }
    return 0;
}

int cb_text(MD_TEXTTYPE type, const MD_CHAR* text, MD_SIZE size, void* ud)
{
    auto& st = *static_cast<MdTtsState*>(ud);
    if (suppressing(st) || type == MD_TEXT_NULLCHAR || !text || size == 0)
        return 0;
    if (type == MD_TEXT_HTML || type == MD_TEXT_CODE || type == MD_TEXT_LATEXMATH)
        return 0;
    append_text(st, text, size);
    if (type == MD_TEXT_SOFTBR || type == MD_TEXT_BR)
        append_break(st);
    return 0;
}

std::string extract_speakable_markdown(std::string_view markdown)
{
    MdTtsState st;
    MD_PARSER parser{};
    parser.abi_version = 0;
    parser.flags = MD_FLAG_TABLES | MD_FLAG_STRIKETHROUGH | MD_FLAG_TASKLISTS
                   | MD_FLAG_COLLAPSEWHITESPACE | MD_FLAG_NOHTML;
    parser.enter_block = cb_enter_block;
    parser.leave_block = cb_leave_block;
    parser.enter_span = cb_enter_span;
    parser.leave_span = cb_leave_span;
    parser.text = cb_text;
    const int rc = md_parse(markdown.data(), static_cast<MD_SIZE>(markdown.size()), &parser, &st);
    if (rc != 0)
        return strip_md_inline(markdown);
    collapse_ws_for_speech(st.out);
    return st.out;
}
#else
std::string extract_speakable_markdown(std::string_view markdown)
{
    std::string out = strip_md_inline(markdown);
    collapse_ws_for_speech(out);
    return out;
}
#endif

} // namespace

void set_tts_sanitizer(TtsSanitizeFn fn)
{
    std::lock_guard<std::mutex> lock(sanitizer_mutex());
    sanitizer_fn() = fn;
}

bool tts_md_matches(const std::filesystem::path& path, std::string_view content)
{
    if (ext_is_markdown(path))
        return true;
    return looks_like_markdown(content);
}

bool tts_preparse_markdown(const std::filesystem::path& path,
                           std::string_view content,
                           std::string& out,
                           std::string& err)
{
    (void)path;
    err.clear();
    std::string body = markdown_body(content);
    TtsSanitizeFn sanitize = nullptr;
    {
        std::lock_guard<std::mutex> lock(sanitizer_mutex());
        sanitize = sanitizer_fn();
    }
    if (sanitize)
        body = sanitize(body);
    trim_in_place(body);
    if (body.empty()) {
        err = "markdown file has no speakable body text after front matter removal";
        return false;
    }
    out = extract_speakable_markdown(body);
    if (out.empty()) {
        err = "markdown file has no speakable body text after preprocessing";
        return false;
    }
    return true;
}

} // namespace pm::tts
