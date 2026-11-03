#include "pm/audio/subs/subtitle_writer.hpp"
#include "fs/path_sanitizer.hpp"

#include <algorithm>
#include <cctype>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <initializer_list>
#include <sstream>
#include <system_error>
#include "string/string_utils.hpp"

namespace pm::subs {
namespace {

std::string trim_copy(std::string s)
{
    while (!s.empty() && std::isspace(static_cast<unsigned char>(s.front())))
        s.erase(s.begin());
    while (!s.empty() && std::isspace(static_cast<unsigned char>(s.back())))
        s.pop_back();
    return s;
}

std::string strip_markup(std::string s)
{
    // YouTube SRT: UTF-8, no markup. Drop a few common tags.
    std::string out;
    out.reserve(s.size());
    bool in_tag = false;
    for (char c : s) {
        if (c == '<') {
            in_tag = true;
            continue;
        }
        if (c == '>') {
            in_tag = false;
            continue;
        }
        if (!in_tag)
            out.push_back(c);
    }
    return out;
}

void pad2(std::ostringstream& o, int v)
{
    if (v < 10) o << '0';
    o << v;
}

void pad3(std::ostringstream& o, int v)
{
    if (v < 10) o << "00";
    else if (v < 100) o << '0';
    o << v;
}

std::string srt_time(std::int64_t ms)
{
    if (ms < 0) ms = 0;
    const std::int64_t h = ms / 3'600'000;
    ms %= 3'600'000;
    const std::int64_t m = ms / 60'000;
    ms %= 60'000;
    const std::int64_t s = ms / 1000;
    const std::int64_t frac = ms % 1000;
    std::ostringstream o;
    pad2(o, static_cast<int>(h));
    o << ':';
    pad2(o, static_cast<int>(m));
    o << ':';
    pad2(o, static_cast<int>(s));
    o << ',';
    pad3(o, static_cast<int>(frac));
    return o.str();
}

std::string vtt_time(std::int64_t ms)
{
    if (ms < 0) ms = 0;
    const std::int64_t h = ms / 3'600'000;
    ms %= 3'600'000;
    const std::int64_t m = ms / 60'000;
    ms %= 60'000;
    const std::int64_t s = ms / 1000;
    const std::int64_t frac = ms % 1000;
    std::ostringstream o;
    pad2(o, static_cast<int>(h));
    o << ':';
    pad2(o, static_cast<int>(m));
    o << ':';
    pad2(o, static_cast<int>(s));
    o << '.';
    pad3(o, static_cast<int>(frac));
    return o.str();
}

std::string sbv_time(std::int64_t ms)
{
    if (ms < 0) ms = 0;
    const std::int64_t h = ms / 3'600'000;
    ms %= 3'600'000;
    const std::int64_t m = ms / 60'000;
    ms %= 60'000;
    const std::int64_t s = ms / 1000;
    const std::int64_t frac = ms % 1000;
    std::ostringstream o;
    o << h << ':';
    pad2(o, static_cast<int>(m));
    o << ':';
    pad2(o, static_cast<int>(s));
    o << '.';
    pad3(o, static_cast<int>(frac));
    return o.str();
}

std::string join_lines(const std::vector<std::string>& lines)
{
    std::string out;
    for (size_t i = 0; i < lines.size(); ++i) {
        if (i) out.push_back('\n');
        out += lines[i];
    }
    return out;
}

bool write_utf8_file(const std::filesystem::path& path,
                     const std::string& body,
                     std::string& err)
{
    std::ofstream f(path, std::ios::binary | std::ios::trunc);
    if (!f) {
        err = "could not write " + media::path::path_to_utf8(path);
        return false;
    }
    f.write(body.data(), static_cast<std::streamsize>(body.size()));
    if (!f) {
        err = "write failed: " + media::path::path_to_utf8(path);
        return false;
    }
    return true;
}

std::vector<Cue> prepare_cues(const std::vector<Cue>& in, const WriterOptions& opt)
{
    std::vector<Cue> out;
    out.reserve(in.size());
    for (const auto& c : in) {
        std::int64_t t0 = c.t0_ms;
        std::int64_t t1 = c.t1_ms;
        if (t1 <= t0)
            t1 = t0 + 1;
        const auto lines = wrap_cue_text(strip_markup(c.text), opt);
        if (lines.empty())
            continue;
        Cue n;
        n.text = trim_copy(join_lines(lines));
        if (n.text.empty())
            continue;
        n.t0_ms = t0;
        n.t1_ms = t1;
        n.segment_id = c.segment_id;
        out.push_back(std::move(n));
    }
    return out;
}

} // namespace

std::vector<std::string> wrap_cue_text(
    const std::string& text, const WriterOptions& opt)
{
    const int max_chars = std::max(8, opt.max_chars);
    std::string t = trim_copy(text);
    std::replace(t.begin(), t.end(), '\r', ' ');
    std::replace(t.begin(), t.end(), '\n', ' ');
    std::vector<std::string> words;
    std::string cur;
    for (char ch : t) {
        if (std::isspace(static_cast<unsigned char>(ch))) {
            if (!cur.empty()) {
                words.push_back(cur);
                cur.clear();
            }
        } else {
            cur.push_back(ch);
        }
    }
    if (!cur.empty())
        words.push_back(cur);

    std::vector<std::string> lines;
    std::string line;
    for (const auto& w : words) {
        if (line.empty()) {
            line = w;
            continue;
        }
        if (static_cast<int>(line.size() + 1 + w.size()) <= max_chars) {
            line.push_back(' ');
            line += w;
        } else {
            lines.push_back(line);
            line = w;
        }
    }
    if (!line.empty())
        lines.push_back(line);
    if (lines.empty() && !t.empty())
        lines.push_back(t.substr(0, static_cast<size_t>(max_chars)));
    return lines;
}

namespace {

/// Sentence end, ignoring a trailing quote or bracket. The cue is visible
/// from its first word, so a new sentence must not sit on the previous one.
bool ends_sentence(const std::string& s)
{
    size_t i = s.size();
    while (i > 0) {
        const unsigned char c = static_cast<unsigned char>(s[i - 1]);
        if (c == '"' || c == '\'' || c == ')' || c == ']')
            --i;
        else
            break;
    }
    if (i >= 3
        && static_cast<unsigned char>(s[i - 3]) == 0xE2
        && static_cast<unsigned char>(s[i - 2]) == 0x80
        && static_cast<unsigned char>(s[i - 1]) == 0xA6)
        return true;
    if (i == 0)
        return false;
    const unsigned char c = static_cast<unsigned char>(s[i - 1]);
    return c == '.' || c == '?' || c == '!';
}

} // namespace

std::vector<Cue> pack_word_cues(const std::vector<Cue>& words,
                                int max_chars,
                                const std::string& text_fallback)
{
    GrowOptions opt;
    opt.max_chars = max_chars;
    return pack_word_cues(words, opt, text_fallback);
}

std::vector<Cue> pack_word_cues(const std::vector<Cue>& words,
                                const GrowOptions& opt,
                                const std::string& text_fallback)
{
    const int chars = std::clamp(opt.max_chars, k_min_cue_chars, k_max_cue_chars);
    const int lines = std::clamp(opt.max_lines, 1, 4);
    const int vis_words = std::max(1, opt.max_visible_words);
    const std::int64_t phrase_break = std::max(0, opt.phrase_break_ms);
    const std::int64_t seg_pause = std::max(0, opt.segment_pause_ms);
    const std::int64_t update_gap = std::max(0, opt.update_gap_ms);
    const std::int64_t phrase_gap = std::max(0, opt.phrase_gap_ms);
    const std::int64_t hold = std::max(0, opt.final_hold_ms);
    struct Piece {
        std::string text;
        std::int64_t t0_ms = 0;
        std::int64_t t1_ms = 0;
        int segment_id = -1;
    };
    std::vector<Cue> out;
    std::vector<Piece> phrase;
    auto joined = [&](size_t begin, size_t end) {
        std::string s;
        for (size_t k = begin; k <= end; ++k) {
            if (!s.empty() && !std::isspace(static_cast<unsigned char>(s.back())))
                s.push_back(' ');
            s += phrase[k].text;
        }
        return s;
    };
    auto cap_against_next = [&](std::int64_t next_t0) {
        if (out.empty())
            return;
        const std::int64_t room = next_t0 - phrase_gap;
        if (out.back().t1_ms > room)
            out.back().t1_ms = room;
        if (out.back().t1_ms <= out.back().t0_ms)
            out.back().t1_ms = out.back().t0_ms + 1;
    };
    auto emit_phrase = [&]() {
        WriterOptions wrap;
        wrap.max_chars = chars;
        wrap.max_lines = lines;
        size_t i = 0;
        while (i < phrase.size()) {
            size_t j = i;
            // Whisper stamps are centiseconds. A 10 ms step is one tick,
            // not a new word the viewer can see (You're / Can / Here).
            while (j + 1 < phrase.size()
                   && phrase[j + 1].t0_ms <= phrase[j].t0_ms + 10
                   && !ends_sentence(phrase[j].text))
                ++j;
            size_t begin = 0;
            while (begin < j) {
                const int n = static_cast<int>(j - begin + 1);
                const bool too_many = n > vis_words;
                const bool too_wide =
                    static_cast<int>(wrap_cue_text(joined(begin, j), wrap).size()) > lines;
                if (!too_many && !too_wide)
                    break;
                ++begin;
            }
            Cue c;
            c.text = joined(begin, j);
            c.segment_id = phrase[j].segment_id;
            c.t0_ms = phrase[i].t0_ms;
            if (j + 1 < phrase.size())
                c.t1_ms = phrase[j + 1].t0_ms - update_gap;
            else
                c.t1_ms = phrase[j].t1_ms + hold;
            if (c.t1_ms <= c.t0_ms)
                c.t1_ms = c.t0_ms + 1;
            if (!c.text.empty())
                out.push_back(std::move(c));
            i = j + 1;
        }
        phrase.clear();
    };
    for (const auto& w : words) {
        std::string piece = trim_copy(w.text);
        if (piece.empty())
            continue;
        if (!phrase.empty()) {
            const auto& prev = phrase.back();
            const std::int64_t gap = w.t0_ms - prev.t1_ms;
            const bool silence = gap > phrase_break;
            const bool seg = w.segment_id >= 0 && prev.segment_id >= 0
                && w.segment_id != prev.segment_id
                && gap > seg_pause;
            if (silence || seg) {
                emit_phrase();
                cap_against_next(w.t0_ms);
            }
        } else if (!out.empty()) {
            cap_against_next(w.t0_ms);
        }
        Piece p;
        p.text = std::move(piece);
        p.t0_ms = w.t0_ms;
        p.t1_ms = w.t1_ms > w.t0_ms ? w.t1_ms : w.t0_ms;
        p.segment_id = w.segment_id;
        const bool sentence = ends_sentence(p.text);
        phrase.push_back(std::move(p));
        if (sentence)
            emit_phrase();
    }
    emit_phrase();
    if (out.empty() && !text_fallback.empty()) {
        Cue c;
        c.text = text_fallback;
        if (!words.empty()) {
            c.t0_ms = words.front().t0_ms;
            c.t1_ms = words.back().t1_ms + hold;
            if (c.t1_ms <= c.t0_ms)
                c.t1_ms = c.t0_ms + 1;
        } else {
            c.t1_ms = 1000;
        }
        out.push_back(std::move(c));
    }
    return out;
}

bool write_srt(const std::filesystem::path& path,
               const std::vector<Cue>& cues,
               const WriterOptions& opt,
               std::string& err)
{
    const auto prepared = prepare_cues(cues, opt);
    std::ostringstream o;
    int idx = 1;
    for (const auto& c : prepared) {
        o << idx++ << "\n"
          << srt_time(c.t0_ms) << " --> " << srt_time(c.t1_ms) << "\n"
          << c.text << "\n\n";
    }
    return write_utf8_file(path, o.str(), err);
}

bool write_vtt(const std::filesystem::path& path,
               const std::vector<Cue>& cues,
               const WriterOptions& opt,
               std::string& err)
{
    const auto prepared = prepare_cues(cues, opt);
    std::ostringstream o;
    o << "WEBVTT\n\n";
    for (const auto& c : prepared) {
        o << vtt_time(c.t0_ms) << " --> " << vtt_time(c.t1_ms) << "\n"
          << c.text << "\n\n";
    }
    return write_utf8_file(path, o.str(), err);
}

bool write_sbv(const std::filesystem::path& path,
               const std::vector<Cue>& cues,
               const WriterOptions& opt,
               std::string& err)
{
    const auto prepared = prepare_cues(cues, opt);
    std::ostringstream o;
    for (const auto& c : prepared) {
        o << sbv_time(c.t0_ms) << ',' << sbv_time(c.t1_ms) << "\n"
          << c.text << "\n\n";
    }
    return write_utf8_file(path, o.str(), err);
}

bool write_format(Format fmt,
                  const std::filesystem::path& path,
                  const std::vector<Cue>& cues,
                  const WriterOptions& opt,
                  std::string& err)
{
    switch (fmt) {
    case Format::vtt: return write_vtt(path, cues, opt, err);
    case Format::sbv: return write_sbv(path, cues, opt, err);
    case Format::srt:
    default:          return write_srt(path, cues, opt, err);
    }
}

std::vector<Format> parse_format_list(std::string token)
{
    token = media::strutil::lower_ascii(std::move(token));
    if (token.empty() || token == "off" || token == "none" || token == "false")
        return {};
    if (token == "all")
        return {Format::srt, Format::vtt, Format::sbv};
    if (token == "vtt" || token == "webvtt")
        return {Format::vtt};
    if (token == "sbv" || token == "subviewer")
        return {Format::sbv};
    if (token == "sidecar" || token == "srt" || token == "on" || token == "true")
        return {Format::srt};
    return {Format::srt};
}

const char* format_ext(Format fmt) noexcept
{
    switch (fmt) {
    case Format::vtt: return ".vtt";
    case Format::sbv: return ".sbv";
    case Format::srt:
    default:          return ".srt";
    }
}

namespace {

bool stem_ext_is(const std::filesystem::path& stem, std::initializer_list<const char*> exts)
{
    std::string e = media::path::path_to_utf8(stem.extension());
    e = media::strutil::lower_ascii(std::move(e));
    for (const char* x : exts) {
        if (e == x)
            return true;
    }
    return false;
}

void strip_known_stem_ext(std::filesystem::path& stem)
{
    if (stem_ext_is(stem, {".wav", ".mp3", ".m4a", ".aac", ".flac", ".ogg", ".opus",
                           ".mp4", ".mkv", ".mov", ".webm", ".m4v", ".wma",
                           ".srt", ".vtt", ".sbv"}))
        stem.replace_extension();
}

} // namespace

bool write_sidecars(std::string format_token,
                    std::filesystem::path stem,
                    const std::string& transcript,
                    std::vector<Cue> cues,
                    WriterOptions opt,
                    std::vector<std::filesystem::path>& written,
                    std::string& err)
{
    written.clear();
    const auto fmts = parse_format_list(std::move(format_token));
    if (fmts.empty())
        return true;
    if (cues.empty() && !transcript.empty()) {
        Cue c;
        c.text = transcript;
        c.t0_ms = 0;
        c.t1_ms = opt.untimed_end_ms > 0 ? opt.untimed_end_ms : 1000;
        cues.push_back(std::move(c));
    }
    if (cues.empty())
        return true;
    if (opt.max_chars < k_min_cue_chars)
        opt.max_chars = k_min_cue_chars;
    if (opt.max_chars > k_max_cue_chars)
        opt.max_chars = k_max_cue_chars;
    if (opt.max_lines < 1)
        opt.max_lines = 1;
    if (opt.max_lines > 4)
        opt.max_lines = 4;
    strip_known_stem_ext(stem);
    if (auto par = stem.parent_path(); !par.empty()) {
        std::error_code ec;
        std::filesystem::create_directories(par, ec);
    }
    for (const auto fmt : fmts) {
        auto out = stem;
        out += format_ext(fmt);
        if (!write_format(fmt, out, cues, opt, err))
            return false;
        written.push_back(std::move(out));
    }
    return true;
}

namespace {

bool read_file_utf8(const std::filesystem::path& path, std::string& text, std::string& err)
{
    std::ifstream f(path, std::ios::binary);
    if (!f) {
        err = "cannot open subtitle file";
        return false;
    }
    std::ostringstream o;
    o << f.rdbuf();
    text = o.str();
    if (text.size() >= 3
        && static_cast<unsigned char>(text[0]) == 0xEF
        && static_cast<unsigned char>(text[1]) == 0xBB
        && static_cast<unsigned char>(text[2]) == 0xBF)
        text.erase(0, 3);
    return true;
}

std::vector<std::string> split_lines(const std::string& text)
{
    std::vector<std::string> lines;
    std::string cur;
    for (char c : text) {
        if (c == '\n') {
            if (!cur.empty() && cur.back() == '\r')
                cur.pop_back();
            lines.push_back(std::move(cur));
            cur.clear();
        } else {
            cur.push_back(c);
        }
    }
    if (!cur.empty() && cur.back() == '\r')
        cur.pop_back();
    if (!cur.empty() || !lines.empty())
        lines.push_back(std::move(cur));
    return lines;
}

bool parse_timestamp(const std::string& token, std::int64_t& ms)
{
    std::string t = trim_copy(token);
    int h = 0, m = 0, s = 0, frac = 0, frac_digits = 0;
    const char* p = t.c_str();
    auto read_int = [&](int& out) -> bool {
        if (*p < '0' || *p > '9')
            return false;
        out = 0;
        while (*p >= '0' && *p <= '9') {
            out = out * 10 + (*p - '0');
            ++p;
        }
        return true;
    };
    int a = 0;
    if (!read_int(a))
        return false;
    if (*p != ':')
        return false;
    ++p;
    int b = 0;
    if (!read_int(b))
        return false;
    if (*p == ':') {
        ++p;
        h = a;
        m = b;
        if (!read_int(s))
            return false;
    } else {
        h = 0;
        m = a;
        s = b;
    }
    if (*p == ',' || *p == '.') {
        ++p;
        while (*p >= '0' && *p <= '9' && frac_digits < 3) {
            frac = frac * 10 + (*p - '0');
            ++p;
            ++frac_digits;
        }
        while (frac_digits < 3) {
            frac *= 10;
            ++frac_digits;
        }
    }
    if (m < 0 || m > 59 || s < 0 || s > 59)
        return false;
    ms = static_cast<std::int64_t>(h) * 3'600'000
       + static_cast<std::int64_t>(m) * 60'000
       + static_cast<std::int64_t>(s) * 1000
       + frac;
    return true;
}

bool parse_time_arrow_line(const std::string& line, std::int64_t& t0, std::int64_t& t1)
{
    const auto arrow = line.find("-->");
    if (arrow == std::string::npos)
        return false;
    std::string left = trim_copy(line.substr(0, arrow));
    std::string right = trim_copy(line.substr(arrow + 3));
    const auto sp = right.find_first_of(" \t");
    if (sp != std::string::npos)
        right = right.substr(0, sp);
    return parse_timestamp(left, t0) && parse_timestamp(right, t1);
}

bool looks_like_index(const std::string& s)
{
    if (s.empty())
        return false;
    for (char c : s) {
        if (c < '0' || c > '9')
            return false;
    }
    return true;
}

} // namespace

bool read_srt(const std::filesystem::path& path, std::vector<Cue>& cues, std::string& err)
{
    std::string text;
    if (!read_file_utf8(path, text, err))
        return false;
    const auto lines = split_lines(text);
    cues.clear();
    size_t i = 0;
    while (i < lines.size()) {
        while (i < lines.size() && trim_copy(lines[i]).empty())
            ++i;
        if (i >= lines.size())
            break;
        if (looks_like_index(trim_copy(lines[i])))
            ++i;
        if (i >= lines.size())
            break;
        std::int64_t t0 = 0, t1 = 0;
        if (!parse_time_arrow_line(lines[i], t0, t1)) {
            ++i;
            continue;
        }
        ++i;
        std::string body;
        while (i < lines.size() && !trim_copy(lines[i]).empty()) {
            if (!body.empty())
                body.push_back('\n');
            body += strip_markup(lines[i]);
            ++i;
        }
        body = trim_copy(body);
        if (!body.empty())
            cues.push_back(Cue{body, t0, t1});
    }
    if (cues.empty()) {
        err = "no SRT cues found";
        return false;
    }
    return true;
}

bool read_vtt(const std::filesystem::path& path, std::vector<Cue>& cues, std::string& err)
{
    std::string text;
    if (!read_file_utf8(path, text, err))
        return false;
    const auto lines = split_lines(text);
    cues.clear();
    size_t i = 0;
    if (i < lines.size()) {
        std::string head = trim_copy(lines[i]);
        head = media::strutil::lower_ascii(std::move(head));
        if (head.rfind("webvtt", 0) == 0)
            ++i;
    }
    while (i < lines.size()) {
        while (i < lines.size() && trim_copy(lines[i]).empty())
            ++i;
        if (i >= lines.size())
            break;
        std::string look = trim_copy(lines[i]);
        std::string look_l = look;
        look_l = media::strutil::lower_ascii(std::move(look_l));
        if (look_l.rfind("note", 0) == 0 || look_l.rfind("style", 0) == 0
            || look_l.rfind("region", 0) == 0) {
            ++i;
            while (i < lines.size() && !trim_copy(lines[i]).empty())
                ++i;
            continue;
        }
        if (look.find("-->") == std::string::npos) {
            ++i;
            if (i >= lines.size())
                break;
            look = lines[i];
        }
        std::int64_t t0 = 0, t1 = 0;
        if (!parse_time_arrow_line(look, t0, t1)) {
            ++i;
            continue;
        }
        ++i;
        std::string body;
        while (i < lines.size() && !trim_copy(lines[i]).empty()) {
            if (!body.empty())
                body.push_back('\n');
            body += strip_markup(lines[i]);
            ++i;
        }
        body = trim_copy(body);
        if (!body.empty())
            cues.push_back(Cue{body, t0, t1});
    }
    if (cues.empty()) {
        err = "no VTT cues found";
        return false;
    }
    return true;
}

bool read_cues_file(const std::filesystem::path& path, std::vector<Cue>& cues, std::string& err)
{
    std::string ext = media::path::path_to_utf8(path.extension());
    ext = media::strutil::lower_ascii(std::move(ext));
    if (ext == ".vtt")
        return read_vtt(path, cues, err);
    if (ext == ".srt")
        return read_srt(path, cues, err);
    std::string first_err;
    if (read_srt(path, cues, first_err))
        return true;
    if (read_vtt(path, cues, err))
        return true;
    err = first_err.empty() ? err : first_err;
    return false;
}

} // namespace pm::subs
