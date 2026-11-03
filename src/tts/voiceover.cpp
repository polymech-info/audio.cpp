#include "pm/audio/tts/voiceover.hpp"
#include "fs/path_sanitizer.hpp"

#include "pm/audio/log.hpp"

#include <filesystem>
#include <string>

#if defined(_MSC_VER)
#  pragma warning(push, 0)
#endif
#include "miniaudio.h"
#if defined(_MSC_VER)
#  pragma warning(pop)
#endif

#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <ios>
#include <sstream>
#include <string>
#include "string/string_utils.hpp"

namespace pm::tts::voiceover {
namespace {

constexpr std::uint64_t k_fnv_off = 1469598103934665603ull;
constexpr std::uint64_t k_fnv_p   = 1099511628211ull;

using media::strutil::lower_ascii;

std::string trim_copy(std::string s)
{
    while (!s.empty() && std::isspace(static_cast<unsigned char>(s.front())))
        s.erase(s.begin());
    while (!s.empty() && std::isspace(static_cast<unsigned char>(s.back())))
        s.pop_back();
    return s;
}

char last_significant_char(const std::string& t)
{
    for (std::size_t i = t.size(); i-- > 0;) {
        const unsigned char c = static_cast<unsigned char>(t[i]);
        if (std::isspace(c) || c == '"' || c == '\'' || c == ')' || c == ']')
            continue;
        return static_cast<char>(c);
    }
    return 0;
}

std::string first_word_lower(const std::string& t)
{
    std::size_t i = 0;
    while (i < t.size() && std::isspace(static_cast<unsigned char>(t[i])))
        ++i;
    std::string w;
    while (i < t.size()) {
        const unsigned char c = static_cast<unsigned char>(t[i]);
        if (std::isspace(c) || c == ',' || c == '.' || c == '!' || c == '?' || c == ':')
            break;
        w.push_back(static_cast<char>(std::tolower(c)));
        ++i;
    }
    return w;
}

bool ends_sentence(const std::string& t)
{
    const char c = last_significant_char(t);
    return c == '.' || c == '!' || c == '?';
}

bool starts_lowercase_word(const std::string& t)
{
    std::size_t i = 0;
    while (i < t.size() && std::isspace(static_cast<unsigned char>(t[i])))
        ++i;
    if (i >= t.size())
        return false;
    const unsigned char c = static_cast<unsigned char>(t[i]);
    return std::islower(c) != 0;
}

bool starts_topic_shift(const std::string& t)
{
    const std::string w = first_word_lower(t);
    static const char* k_words[] = {
        "next", "then", "now", "first", "finally", "however", "also",
        "meanwhile", "after", "before", "so", "anyway", "instead",
        "plus", "second", "third", "lastly", "afterwards",
    };
    for (const char* k : k_words) {
        if (w == k)
            return true;
    }
    if (w == "let's" || w == "lets")
        return true;
    return false;
}

int boundary_score(const std::string& prev, const std::string& next, int gap_ms)
{
    int score = gap_ms;
    if (ends_sentence(prev))
        score += 400;
    else {
        const char c = last_significant_char(prev);
        if (c == ',' || c == ';' || c == ':')
            score -= 150;
        else
            score -= 250;
    }
    if (starts_topic_shift(next))
        score += 500;
    if (starts_lowercase_word(next))
        score -= 200;
    return score;
}

int estimated_duration_ms(const std::string& text)
{
    // ~13 chars/s conversational English, floor 400 ms.
    const int ms = static_cast<int>(text.size()) * 75;
    return ms < 400 ? 400 : ms;
}

std::int64_t pcm_duration_ms(const std::vector<std::int16_t>& pcm)
{
    if (pcm.empty())
        return 0;
    return (static_cast<std::int64_t>(pcm.size()) * 1000)
         / C::k_voiceover_sample_rate;
}

std::uint64_t fnv1a(const std::string& s)
{
    std::uint64_t h = k_fnv_off;
    for (unsigned char c : s) {
        h ^= c;
        h *= k_fnv_p;
    }
    return h;
}

std::string hex64(std::uint64_t v)
{
    std::ostringstream o;
    o << std::hex << v;
    return o.str();
}

std::int64_t derived_seed(const std::string& text, const Options& opt)
{
    if (opt.seed >= 0)
        return opt.seed;
    const std::uint64_t h = fnv1a(opt.provider + "|" + opt.model + "|" + opt.voice + "|" + text);
    return static_cast<std::int64_t>(h & 0xFFFFFFFFull);
}

std::vector<std::uint8_t> wrap_pcm_wav(const std::uint8_t* pcm, std::size_t n, int sample_rate)
{
    std::vector<std::uint8_t> wav(44 + n);
    const std::uint32_t data_bytes = static_cast<std::uint32_t>(n);
    const std::uint32_t sr = static_cast<std::uint32_t>(sample_rate);
    auto w16 = [&](std::size_t off, std::uint16_t v) {
        wav[off] = static_cast<std::uint8_t>(v);
        wav[off + 1] = static_cast<std::uint8_t>(v >> 8);
    };
    auto w32 = [&](std::size_t off, std::uint32_t v) {
        wav[off] = static_cast<std::uint8_t>(v);
        wav[off + 1] = static_cast<std::uint8_t>(v >> 8);
        wav[off + 2] = static_cast<std::uint8_t>(v >> 16);
        wav[off + 3] = static_cast<std::uint8_t>(v >> 24);
    };
    std::memcpy(wav.data(), "RIFF", 4);
    w32(4, 36 + data_bytes);
    std::memcpy(wav.data() + 8, "WAVEfmt ", 8);
    w32(16, 16);
    w16(20, 1);
    w16(22, 1);
    w32(24, sr);
    w32(28, sr * 2);
    w16(32, 2);
    w16(34, 16);
    std::memcpy(wav.data() + 36, "data", 4);
    w32(40, data_bytes);
    if (n)
        std::memcpy(wav.data() + 44, pcm, n);
    return wav;
}

bool decode_memory_to_48k_mono(const void* data, std::size_t size,
                               std::vector<std::int16_t>& pcm, std::string& err)
{
    ma_decoder_config cfg = ma_decoder_config_init(ma_format_s16, 1, C::k_voiceover_sample_rate);
    ma_decoder decoder{};
    const ma_result init = ma_decoder_init_memory(data, size, &cfg, &decoder);
    if (init != MA_SUCCESS) {
        err = "cannot decode TTS audio";
        return false;
    }
    ma_uint64 frames = 0;
    if (ma_decoder_get_length_in_pcm_frames(&decoder, &frames) != MA_SUCCESS || frames == 0) {
        std::vector<std::int16_t> chunk(static_cast<std::size_t>(C::k_voiceover_sample_rate));
        for (;;) {
            ma_uint64 got = 0;
            if (ma_decoder_read_pcm_frames(&decoder, chunk.data(), chunk.size(), &got) != MA_SUCCESS)
                break;
            if (got == 0)
                break;
            pcm.insert(pcm.end(), chunk.begin(), chunk.begin() + static_cast<std::size_t>(got));
        }
    } else {
        pcm.resize(static_cast<std::size_t>(frames));
        ma_uint64 got = 0;
        const ma_result read = ma_decoder_read_pcm_frames(&decoder, pcm.data(), frames, &got);
        if (read != MA_SUCCESS || got == 0) {
            ma_decoder_uninit(&decoder);
            err = "failed to read TTS frames";
            return false;
        }
        pcm.resize(static_cast<std::size_t>(got));
    }
    ma_decoder_uninit(&decoder);
    if (pcm.empty()) {
        err = "decoded TTS audio is empty";
        return false;
    }
    return true;
}

void trim_generated_silence(std::vector<std::int16_t>& pcm)
{
    if (pcm.empty())
        return;
    const int pad = (C::k_voiceover_trim_pad_ms * C::k_voiceover_sample_rate) / 1000;
    const int thresh = C::k_voiceover_silence_thresh;
    std::size_t begin = 0;
    while (begin < pcm.size() && std::abs(static_cast<int>(pcm[begin])) < thresh)
        ++begin;
    std::size_t end = pcm.size();
    while (end > begin && std::abs(static_cast<int>(pcm[end - 1])) < thresh)
        --end;
    if (begin > static_cast<std::size_t>(pad))
        begin -= static_cast<std::size_t>(pad);
    else
        begin = 0;
    if (end + static_cast<std::size_t>(pad) < pcm.size())
        end += static_cast<std::size_t>(pad);
    else
        end = pcm.size();
    if (begin == 0 && end == pcm.size())
        return;
    pcm.assign(pcm.begin() + static_cast<std::ptrdiff_t>(begin),
               pcm.begin() + static_cast<std::ptrdiff_t>(end));
}

std::vector<std::int16_t> time_compress(const std::vector<std::int16_t>& in, std::size_t out_n)
{
    std::vector<std::int16_t> out(out_n, 0);
    if (in.empty() || out_n == 0)
        return out;
    if (out_n == in.size())
        return in;
    for (std::size_t i = 0; i < out_n; ++i) {
        const double src = (out_n == 1)
            ? 0.0
            : (static_cast<double>(i) * static_cast<double>(in.size() - 1)
               / static_cast<double>(out_n - 1));
        const std::size_t a = static_cast<std::size_t>(src);
        const std::size_t b = (a + 1 < in.size()) ? a + 1 : a;
        const double t = src - static_cast<double>(a);
        const double va = static_cast<double>(in[a]);
        const double vb = static_cast<double>(in[b]);
        out[i] = static_cast<std::int16_t>(va + (vb - va) * t);
    }
    return out;
}

bool write_s16_wav_48k(const std::filesystem::path& path,
                       const std::int16_t* samples,
                       std::size_t n_samples,
                       std::string& err)
{
    std::ofstream f(path, std::ios::binary | std::ios::trunc);
    if (!f) {
        err = "cannot write voiceover wav";
        return false;
    }
    const std::uint32_t data_bytes = static_cast<std::uint32_t>(n_samples * sizeof(std::int16_t));
    const std::uint32_t sample_rate = C::k_voiceover_sample_rate;
    auto w16 = [&](std::uint16_t v) { f.write(reinterpret_cast<const char*>(&v), 2); };
    auto w32 = [&](std::uint32_t v) { f.write(reinterpret_cast<const char*>(&v), 4); };
    f.write("RIFF", 4); w32(36 + data_bytes);
    f.write("WAVE", 4);
    f.write("fmt ", 4); w32(16);
    w16(1);
    w16(1);
    w32(sample_rate);
    w32(sample_rate * 2);
    w16(2);
    w16(16);
    f.write("data", 4); w32(data_bytes);
    f.write(reinterpret_cast<const char*>(samples),
            static_cast<std::streamsize>(data_bytes));
    if (!f) {
        err = "voiceover wav write failed";
        return false;
    }
    return true;
}

bool cues_from_json_text(const std::string& text, std::vector<pm::subs::Cue>& cues, std::string& err)
{
    nlohmann::json j;
    try {
        j = nlohmann::json::parse(text);
    } catch (const std::exception& ex) {
        err = std::string("not JSON cues: ") + ex.what();
        return false;
    }
    nlohmann::json arr;
    if (j.is_array())
        arr = j;
    else if (j.is_object() && j.contains("segments") && j["segments"].is_array())
        arr = j["segments"];
    else if (j.is_object() && j.contains("cues") && j["cues"].is_array())
        arr = j["cues"];
    else {
        err = "JSON cues need an array, or {segments|cues}";
        return false;
    }
    cues.clear();
    for (const auto& item : arr) {
        if (!item.is_object())
            continue;
        pm::subs::Cue c;
        if (item.contains("text") && item["text"].is_string())
            c.text = item["text"].get<std::string>();
        auto pick_ms = [&](const char* a, const char* b) -> std::int64_t {
            if (item.contains(a) && item[a].is_number())
                return item[a].get<std::int64_t>();
            if (item.contains(b) && item[b].is_number())
                return item[b].get<std::int64_t>();
            return 0;
        };
        c.t0_ms = pick_ms("t0Ms", "t0_ms");
        c.t1_ms = pick_ms("t1Ms", "t1_ms");
        if (c.t1_ms <= c.t0_ms && item.contains("t1_cs"))
            c.t1_ms = item.value("t1_cs", 0) * 10;
        if (c.t0_ms == 0 && item.contains("t0_cs"))
            c.t0_ms = item.value("t0_cs", 0) * 10;
        c.text = trim_copy(c.text);
        if (!c.text.empty())
            cues.push_back(std::move(c));
    }
    if (cues.empty()) {
        err = "no cues in JSON";
        return false;
    }
    return true;
}

std::string cache_key(const SynthesizeRequest& req, const Options& opt)
{
    std::ostringstream o;
    o << opt.provider << '\n' << opt.model << '\n' << opt.voice << '\n'
      << opt.cache_extra << '\n'
      << req.text << '\n' << req.previous_text << '\n' << req.next_text << '\n'
      << req.speed << '\n' << req.seed << '\n' << req.alignment;
    return hex64(fnv1a(o.str()));
}

bool cache_load(const std::filesystem::path& dir, const std::string& key, VoiceOverTake& take)
{
    const auto pcm_path = dir / (key + ".s16");
    const auto meta_path = dir / (key + ".json");
    std::error_code ec;
    if (!std::filesystem::is_regular_file(pcm_path, ec) || !std::filesystem::is_regular_file(meta_path, ec))
        return false;
    const auto sz = std::filesystem::file_size(pcm_path, ec);
    if (ec || sz == 0 || (sz % 2) != 0)
        return false;
    std::ifstream f(pcm_path, std::ios::binary);
    if (!f)
        return false;
    take.pcm.resize(static_cast<std::size_t>(sz / 2));
    f.read(reinterpret_cast<char*>(take.pcm.data()), static_cast<std::streamsize>(sz));
    if (!f)
        return false;
    try {
        std::ifstream mf(meta_path);
        nlohmann::json j;
        mf >> j;
        take.request_id = j.value("request_id", std::string{});
        take.seed = j.value("seed", static_cast<std::int64_t>(-1));
        take.generation_speed = j.value("speed", 1.0f);
        take.duration_ms = pcm_duration_ms(take.pcm);
        take.from_cache = true;
        return true;
    } catch (...) {
        return false;
    }
}

void cache_store(const std::filesystem::path& dir, const std::string& key, const VoiceOverTake& take)
{
    std::error_code ec;
    std::filesystem::create_directories(dir, ec);
    const auto pcm_path = dir / (key + ".s16");
    const auto meta_path = dir / (key + ".json");
    {
        std::ofstream f(pcm_path, std::ios::binary | std::ios::trunc);
        if (!f)
            return;
        f.write(reinterpret_cast<const char*>(take.pcm.data()),
                static_cast<std::streamsize>(take.pcm.size() * sizeof(std::int16_t)));
    }
    nlohmann::json j = {
        {"request_id", take.request_id},
        {"seed", take.seed},
        {"speed", take.generation_speed},
        {"duration_ms", take.duration_ms},
        {"sample_rate", C::k_voiceover_sample_rate},
    };
    std::ofstream mf(meta_path, std::ios::trunc);
    mf << j.dump(2);
}

void mix_into(std::vector<std::int16_t>& bed, std::int64_t t0_ms,
              const std::vector<std::int16_t>& pcm)
{
    if (pcm.empty())
        return;
    const std::int64_t off = (t0_ms * C::k_voiceover_sample_rate) / 1000;
    if (off < 0)
        return;
    const std::size_t start = static_cast<std::size_t>(off);
    if (start + pcm.size() > bed.size())
        bed.resize(start + pcm.size(), 0);
    for (std::size_t i = 0; i < pcm.size(); ++i) {
        const int v = static_cast<int>(bed[start + i]) + static_cast<int>(pcm[i]);
        if (v > 32767)
            bed[start + i] = 32767;
        else if (v < -32768)
            bed[start + i] = -32768;
        else
            bed[start + i] = static_cast<std::int16_t>(v);
    }
}

} // namespace

bool decode_tts_bytes(const std::vector<std::uint8_t>& bytes,
                      const std::string& format_hint,
                      std::vector<std::int16_t>& pcm48_mono,
                      std::string& err)
{
    pcm48_mono.clear();
    if (bytes.empty()) {
        err = "empty TTS bytes";
        return false;
    }
    std::string fmt = lower_ascii(format_hint);
    if (fmt.size() >= 4 && fmt.compare(0, 4, "pcm_") == 0) {
        int sr = 22050;
        try {
            sr = std::stoi(fmt.substr(4));
        } catch (...) {
            sr = 22050;
        }
        if (sr <= 0)
            sr = 22050;
        const auto wav = wrap_pcm_wav(bytes.data(), bytes.size(), sr);
        return decode_memory_to_48k_mono(wav.data(), wav.size(), pcm48_mono, err);
    }
    return decode_memory_to_48k_mono(bytes.data(), bytes.size(), pcm48_mono, err);
}

bool load_cues(const std::string& input, std::vector<pm::subs::Cue>& cues, std::string& err)
{
    const std::string trimmed = trim_copy(input);
    if (trimmed.empty()) {
        err = "empty script input";
        return false;
    }
    std::error_code ec;
    const std::filesystem::path path(trimmed);
    if (std::filesystem::is_regular_file(path, ec) && !ec) {
        std::string ext = media::path::path_to_utf8(path.extension());
        ext = lower_ascii(ext);
        if (ext == ".json") {
            std::ifstream f(path, std::ios::binary);
            std::ostringstream o;
            o << f.rdbuf();
            return cues_from_json_text(o.str(), cues, err);
        }
        return pm::subs::read_cues_file(path, cues, err);
    }
    if (!trimmed.empty() && (trimmed.front() == '{' || trimmed.front() == '['))
        return cues_from_json_text(trimmed, cues, err);
    err = "script input is not an existing SRT/VTT/JSON file";
    return false;
}

namespace {

// Grow-style sidecars emit every partial ("Das", "Das hat", "Das hat sie gesagt.").
// Each update is the previous text plus more, and the cues meet within one frame.
// Speak the finished line once, from the first partial's start to the last end.
std::vector<pm::subs::Cue> collapse_grow_cues(const std::vector<pm::subs::Cue>& cues)
{
    constexpr std::int64_t k_max_update_gap_ms = 20;
    std::vector<pm::subs::Cue> out;
    out.reserve(cues.size());
    for (const auto& cue : cues) {
        const std::string text = trim_copy(cue.text);
        if (text.empty())
            continue;
        if (!out.empty()) {
            auto& prev = out.back();
            const std::int64_t gap = cue.t0_ms - prev.t1_ms;
            if (gap <= k_max_update_gap_ms
                && text.size() > prev.text.size()
                && text.compare(0, prev.text.size(), prev.text) == 0) {
                prev.text = text;
                if (cue.t1_ms > prev.t1_ms)
                    prev.t1_ms = cue.t1_ms;
                continue;
            }
        }
        pm::subs::Cue kept = cue;
        kept.text = text;
        out.push_back(std::move(kept));
    }
    return out;
}

} // namespace

std::vector<VoiceOverUtterance> merge_utterances(
    const std::vector<pm::subs::Cue>& cues, const Options& opt)
{
    std::vector<VoiceOverUtterance> out;
    const std::vector<pm::subs::Cue> grown = collapse_grow_cues(cues);
    if (grown.size() != cues.size()) {
        pm::audio::log::info("[voiceover] collapsed grow cues "
                     + std::to_string(cues.size()) + " -> "
                     + std::to_string(grown.size()));
    }
    if (grown.empty())
        return out;
    VoiceOverUtterance cur;
    cur.t0_ms = grown[0].t0_ms;
    cur.nominal_t1_ms = grown[0].t1_ms;
    cur.text = grown[0].text;
    cur.cue_indices.push_back(0);

    auto flush = [&] {
        if (!cur.text.empty())
            out.push_back(cur);
        cur = {};
    };

    for (std::size_t i = 1; i < grown.size(); ++i) {
        const auto& next = grown[i];
        const int gap = static_cast<int>(next.t0_ms - cur.nominal_t1_ms);
        std::string combined = cur.text;
        if (!combined.empty() && !next.text.empty())
            combined.push_back(' ');
        combined += next.text;
        const bool over_len = estimated_duration_ms(combined) > opt.max_utterance_ms;
        const int score = boundary_score(cur.text, next.text, gap < 0 ? 0 : gap);
        const bool merge = !over_len && score <= opt.merge_score_limit;
        pm::audio::log::trace("[voiceover] merge? gap=" + std::to_string(gap)
                      + "ms score=" + std::to_string(score)
                      + " limit=" + std::to_string(opt.merge_score_limit)
                      + " -> " + (merge ? "yes" : "no"));
        if (merge) {
            cur.nominal_t1_ms = next.t1_ms;
            if (!cur.text.empty() && !next.text.empty())
                cur.text.push_back(' ');
            cur.text += next.text;
            cur.cue_indices.push_back(i);
        } else {
            flush();
            cur.t0_ms = next.t0_ms;
            cur.nominal_t1_ms = next.t1_ms;
            cur.text = next.text;
            cur.cue_indices = {i};
        }
    }
    flush();

    const int gap = opt.min_inter_gap_ms > 0 ? opt.min_inter_gap_ms : C::k_voiceover_min_inter_gap_ms;
    for (std::size_t i = 0; i < out.size(); ++i) {
        if (i + 1 < out.size())
            out[i].hard_t1_ms = out[i + 1].t0_ms - gap;
        else
            out[i].hard_t1_ms = out[i].nominal_t1_ms + C::k_voiceover_last_pad_ms;
        if (out[i].hard_t1_ms < out[i].t0_ms + 200)
            out[i].hard_t1_ms = out[i].t0_ms + 200;
        pm::audio::log::trace("[voiceover] utterance " + std::to_string(i)
                      + " t0=" + std::to_string(out[i].t0_ms)
                      + " nominal=" + std::to_string(out[i].nominal_t1_ms)
                      + " hard=" + std::to_string(out[i].hard_t1_ms)
                      + " text=\"" + out[i].text + "\"");
    }
    return out;
}

nlohmann::json report_to_json(const OverflowReport& r)
{
    return {
        {"utterances", r.utterances},
        {"natural", r.natural},
        {"used_gap", r.used_gap},
        {"stretched", r.stretched},
        {"regenerated", r.regenerated},
        {"unresolved", r.unresolved},
        {"items", r.utterances_json},
    };
}

bool render_timeline(const std::vector<pm::subs::Cue>& cues,
                     const Options& opt,
                     const TtsCapabilities& caps,
                     const SynthesizeFn& synth,
                     const std::filesystem::path& dst_wav,
                     OverflowReport& report,
                     std::string& err)
{
    report = {};
    if (!synth) {
        err = "no TTS synthesizer";
        return false;
    }
    auto utterances = merge_utterances(cues, opt);
    report.utterances = static_cast<int>(utterances.size());
    if (utterances.empty()) {
        err = "no utterances after merge";
        return false;
    }

    std::filesystem::path cache_dir = opt.cache_dir;
    if (cache_dir.empty())
        cache_dir = dst_wav.parent_path() / ".voiceover-cache";

    std::vector<std::string> prev_ids;
    std::vector<std::int16_t> bed;

    auto last_ids = [&]() {
        std::vector<std::string> ids = prev_ids;
        while (ids.size() > 3)
            ids.erase(ids.begin());
        return ids;
    };

    for (std::size_t i = 0; i < utterances.size(); ++i) {
        const auto& u = utterances[i];
        const std::string next_text =
            (i + 1 < utterances.size()) ? utterances[i + 1].text : std::string{};
        const std::string previous_text =
            (i > 0) ? utterances[i - 1].text : std::string{};
        const std::int64_t seed = derived_seed(u.text, opt);
        SynthesizeRequest req;
        req.text = u.text;
        req.previous_text = previous_text;
        req.next_text = next_text;
        // Forward pass: linguistic neighbors. Request IDs suppress previous_text
        // and are reserved for speed-regen stitching.
        req.speed = opt.speed;
        req.seed = seed;
        req.alignment = opt.alignment;
        pm::audio::log::trace("[voiceover] synth utterance=" + std::to_string(i)
                      + " prev_text=" + std::to_string(previous_text.size()) + "c"
                      + " next_text=" + std::to_string(next_text.size()) + "c");

        const std::string key = cache_key(req, opt);
        VoiceOverTake take;
        if (!cache_load(cache_dir, key, take)) {
            take = synth(req);
            take.seed = seed;
            take.generation_speed = req.speed;
            if (take.pcm.empty()) {
                err = "TTS returned empty PCM for utterance " + std::to_string(i);
                return false;
            }
            cache_store(cache_dir, key, take);
        } else {
            pm::audio::log::trace("[voiceover] cache hit utterance=" + std::to_string(i)
                          + " key=" + key);
        }

        const std::int64_t raw_ms = pcm_duration_ms(take.pcm);
        trim_generated_silence(take.pcm);
        take.duration_ms = pcm_duration_ms(take.pcm);

        const std::int64_t budget = u.hard_t1_ms - u.t0_ms;
        const std::int64_t nominal_win = u.nominal_t1_ms - u.t0_ms;
        const std::int64_t free_gap = u.hard_t1_ms > u.nominal_t1_ms
            ? (u.hard_t1_ms - u.nominal_t1_ms) : 0;

        Placement p;
        p.t0_ms = u.t0_ms;
        p.generated_ms = raw_ms;
        p.trimmed_ms = take.duration_ms;
        p.free_gap_ms = free_gap;
        p.original_window_ms = nominal_win > 0 ? nominal_win : 0;
        p.stretch_ratio = 1.0f;

        auto apply_fit = [&](VoiceOverTake& t) {
            const std::int64_t actual = pcm_duration_ms(t.pcm);
            if (actual <= budget) {
                p.t1_ms = u.t0_ms + actual;
                if (actual <= nominal_win) {
                    p.natural = true;
                } else {
                    p.consumed_gap = true;
                }
                return;
            }
            const double overflow = budget > 0
                ? (static_cast<double>(actual) / static_cast<double>(budget) - 1.0)
                : 1.0;
            if (overflow <= opt.max_stretch && budget > 0) {
                const std::size_t target = static_cast<std::size_t>(
                    (budget * C::k_voiceover_sample_rate) / 1000);
                t.pcm = time_compress(t.pcm, target);
                t.duration_ms = pcm_duration_ms(t.pcm);
                p.stretch_ratio = actual > 0
                    ? static_cast<float>(static_cast<double>(actual) / static_cast<double>(budget))
                    : 1.0f;
                p.t1_ms = u.t0_ms + t.duration_ms;
                return;
            }
            if (opt.overflow == OverflowMode::Truncate && budget > 0) {
                const std::size_t target = static_cast<std::size_t>(
                    (budget * C::k_voiceover_sample_rate) / 1000);
                if (t.pcm.size() > target)
                    t.pcm.resize(target);
                t.duration_ms = pcm_duration_ms(t.pcm);
                p.truncated = true;
                p.t1_ms = u.t0_ms + t.duration_ms;
                return;
            }
            p.unresolved = true;
            p.t1_ms = u.t0_ms + actual;
        };

        apply_fit(take);

        const bool overflowed = p.unresolved && caps.speed_control
            && opt.overflow == OverflowMode::Resolve;

        if (overflowed) {
            const std::int64_t actual = pcm_duration_ms(take.pcm);
            float regen_speed = opt.speed;
            if (budget > 0 && actual > budget) {
                regen_speed = static_cast<float>(
                    static_cast<double>(actual) / static_cast<double>(budget));
                if (regen_speed < 1.0f)
                    regen_speed = 1.0f;
                if (regen_speed > 1.2f)
                    regen_speed = 1.2f;
            }
            SynthesizeRequest regen = req;
            regen.speed = regen_speed;
            regen.previous_text.clear();
            regen.next_text.clear();
            if (caps.request_stitching)
                regen.previous_request_ids = last_ids();
            const std::string rkey = cache_key(regen, opt);
            VoiceOverTake again;
            if (!cache_load(cache_dir, rkey, again)) {
                again = synth(regen);
                again.seed = seed;
                again.generation_speed = regen_speed;
                cache_store(cache_dir, rkey, again);
            }
            trim_generated_silence(again.pcm);
            again.duration_ms = pcm_duration_ms(again.pcm);
            p.regenerated = true;
            p.unresolved = false;
            p.natural = false;
            p.consumed_gap = false;
            p.truncated = false;
            p.stretch_ratio = 1.0f;
            apply_fit(again);
            take = std::move(again);
            p.trimmed_ms = take.duration_ms;
        }

        if (p.natural)
            ++report.natural;
        if (p.consumed_gap)
            ++report.used_gap;
        if (p.stretch_ratio > 1.0001f)
            ++report.stretched;
        if (p.regenerated)
            ++report.regenerated;
        if (p.unresolved)
            ++report.unresolved;

        pm::audio::log::info("[voiceover] utterance " + std::to_string(i)
                     + " window=" + std::to_string(p.original_window_ms / 1000.0)
                     + "s generated=" + std::to_string(p.trimmed_ms / 1000.0)
                     + "s gap=" + std::to_string(p.free_gap_ms / 1000.0)
                     + "s stretch=" + std::to_string((p.stretch_ratio - 1.0f) * 100.0f)
                     + "%"
                     + (p.natural ? " natural" : "")
                     + (p.consumed_gap ? " used_gap" : "")
                     + (p.regenerated ? " regenerated" : "")
                     + (p.unresolved ? " unresolved" : ""));

        nlohmann::json item = {
            {"index", static_cast<int>(i)},
            {"text", u.text},
            {"t0Ms", u.t0_ms},
            {"nominalT1Ms", u.nominal_t1_ms},
            {"hardT1Ms", u.hard_t1_ms},
            {"original_window_s", p.original_window_ms / 1000.0},
            {"generated_s", p.trimmed_ms / 1000.0},
            {"raw_generated_s", p.generated_ms / 1000.0},
            {"free_gap_s", p.free_gap_ms / 1000.0},
            {"final_stretch_pct", (p.stretch_ratio - 1.0f) * 100.0f},
            {"placedT0Ms", p.t0_ms},
            {"placedT1Ms", p.t1_ms},
            {"request_id", take.request_id},
            {"seed", take.seed},
            {"speed", take.generation_speed},
            {"natural", p.natural},
            {"used_gap", p.consumed_gap},
            {"stretched", p.stretch_ratio > 1.0001f},
            {"regenerated", p.regenerated},
            {"unresolved", p.unresolved},
            {"truncated", p.truncated},
            {"from_cache", take.from_cache},
        };
        report.utterances_json.push_back(std::move(item));

        if (!take.request_id.empty()) {
            prev_ids.push_back(take.request_id);
            if (prev_ids.size() > 3)
                prev_ids.erase(prev_ids.begin());
        }
        mix_into(bed, p.t0_ms, take.pcm);
    }

    if (bed.empty()) {
        err = "voiceover bed is empty";
        return false;
    }
    std::error_code ec;
    if (auto par = dst_wav.parent_path(); !par.empty())
        std::filesystem::create_directories(par, ec);
    if (!write_s16_wav_48k(dst_wav, bed.data(), bed.size(), err))
        return false;

    const auto report_path = dst_wav.parent_path()
        / (media::path::path_to_utf8(dst_wav.stem()) + ".voiceover.json");
    {
        std::ofstream rf(report_path, std::ios::trunc);
        rf << report_to_json(report).dump(2);
    }
    pm::audio::log::info("[voiceover] wrote " + media::path::path_to_utf8(dst_wav)
                 + " utterances=" + std::to_string(report.utterances)
                 + " natural=" + std::to_string(report.natural)
                 + " used_gap=" + std::to_string(report.used_gap)
                 + " stretched=" + std::to_string(report.stretched)
                 + " regenerated=" + std::to_string(report.regenerated)
                 + " unresolved=" + std::to_string(report.unresolved));
    return true;
}

} // namespace pm::tts::voiceover

