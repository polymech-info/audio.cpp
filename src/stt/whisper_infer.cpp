#include "pm/audio/stt/whisper.hpp"
#include "pm/audio/text.hpp"
#include "pm/audio/stt/wav.hpp"
#include "pm/audio/log.hpp"

#if defined(PM_AUDIO_LINK_WHISPER) && PM_AUDIO_LINK_WHISPER
#include <ggml.h>
#include <ggml-backend.h>
#include <whisper.h>
#endif

enum {
    PM_WHISPER_FLASH_AUTO = 0,
    PM_WHISPER_FLASH_ON = 1,
    PM_WHISPER_FLASH_OFF = 2,
    PM_WHISPER_TASK_API = 2
};

#include <algorithm>
#include <atomic>
#include <cmath>
#include <cctype>
#include <chrono>
#include <cstdint>
#include <cstring>
#include <limits>
#include <filesystem>
#include <fstream>
#include <mutex>
#include <memory>
#include <sstream>
#include <stdexcept>
#include <string>
#include <thread>
#include <vector>

namespace pm::stt {

WhisperGateFn& whisper_gate_slot()
{
    static WhisperGateFn fn = nullptr;
    return fn;
}

WhisperCancelFn& whisper_cancel_slot()
{
    static WhisperCancelFn fn = nullptr;
    return fn;
}

GgmlBackendLoadFn& whisper_ggml_load_slot()
{
    static GgmlBackendLoadFn fn = nullptr;
    return fn;
}

WhisperResolveFn& whisper_resolve_slot()
{
    static WhisperResolveFn fn = nullptr;
    return fn;
}

void set_whisper_gate(WhisperGateFn fn) { whisper_gate_slot() = fn; }
void set_whisper_cancel(WhisperCancelFn fn) { whisper_cancel_slot() = fn; }
void set_ggml_backend_load(GgmlBackendLoadFn fn) { whisper_ggml_load_slot() = fn; }
void set_whisper_resolve(WhisperResolveFn fn) { whisper_resolve_slot() = fn; }


namespace {

namespace fs = std::filesystem;

std::atomic<bool> g_whisper_gpu_unavailable{false};

void note_whisper_gpu_fallback(const std::string& why)
{
    g_whisper_gpu_unavailable.store(true, std::memory_order_relaxed);
    pm::audio::log::warn("[whisper] gpu unavailable, falling back to cpu: " + why);
}

WhisperLocalOptions coerce_whisper_gpu(const WhisperLocalOptions& options, bool retry_gpu)
{
    WhisperLocalOptions out = options;
    if (!out.use_gpu)
        return out;
    if (!retry_gpu && g_whisper_gpu_unavailable.load(std::memory_order_relaxed))
        out.use_gpu = false;
    return out;
}

struct ActiveWhisperModel {
    std::uint64_t id = 0;
    std::string model_path;
    std::string language;
    bool use_gpu = false;
    int gpu_device = 0;
    int threads = 0;
    std::int64_t started_unix_ms = 0;
};

std::mutex& active_whisper_mutex()
{
    static std::mutex m;
    return m;
}

std::vector<ActiveWhisperModel>& active_whisper_models()
{
    static std::vector<ActiveWhisperModel> rows;
    return rows;
}

std::uint64_t next_whisper_active_id()
{
    static std::atomic<std::uint64_t> seq{1};
    return seq.fetch_add(1, std::memory_order_relaxed);
}

std::int64_t now_unix_ms()
{
    return std::chrono::duration_cast<std::chrono::milliseconds>(
               std::chrono::system_clock::now().time_since_epoch())
        .count();
}

class ActiveWhisperScope {
public:
    explicit ActiveWhisperScope(ActiveWhisperModel row) : id_(row.id)
    {
        std::lock_guard<std::mutex> lk(active_whisper_mutex());
        active_whisper_models().push_back(std::move(row));
    }
    ~ActiveWhisperScope()
    {
        std::lock_guard<std::mutex> lk(active_whisper_mutex());
        auto& rows = active_whisper_models();
        rows.erase(std::remove_if(rows.begin(), rows.end(),
                                  [this](const ActiveWhisperModel& row) { return row.id == id_; }),
                   rows.end());
    }
    ActiveWhisperScope(const ActiveWhisperScope&) = delete;
    ActiveWhisperScope& operator=(const ActiveWhisperScope&) = delete;

private:
    std::uint64_t id_;
};

bool path_exists(const std::string& p)
{
    try { return !p.empty() && fs::exists(p); } catch (...) { return false; }
}

bool whisper_full_should_abort(void* user)
{
    if (WhisperCancelFn cancel_fn = whisper_cancel_slot())
        if (cancel_fn())
            return true;
    const auto* cancel = static_cast<const std::atomic_bool*>(user);
    return cancel && cancel->load(std::memory_order_relaxed);
}

#if defined(PM_AUDIO_LINK_WHISPER) && PM_AUDIO_LINK_WHISPER
// Drop whisper/ggml INFO/DEBUG spam (stdout is reserved for NDJSON). Keep WARN
// and ERROR so CUDA/backend failures are visible instead of a silent abort.
void install_whisper_log_once()
{
    static bool done = false;
    if (done) return;
    done = true;
    whisper_log_set([](ggml_log_level level, const char* text, void*) {
        if (!text || !*text || level < GGML_LOG_LEVEL_WARN)
            return;
        std::string msg = text;
        while (!msg.empty() && (msg.back() == '\n' || msg.back() == '\r'))
            msg.pop_back();
        if (msg.empty())
            return;
        if (level >= GGML_LOG_LEVEL_ERROR)
            pm::audio::log::warn("[whisper] " + msg);
        else
            pm::audio::log::warn("[whisper] " + msg);
    }, nullptr);
    ggml_set_abort_callback([](const char* error_message) {
        const char* msg = (error_message && *error_message)
            ? error_message : "(no details)";
        pm::audio::log::warn(std::string("[whisper] ggml abort: ") + msg);
    });
}

void load_ggml_backends_once()
{
    if (GgmlBackendLoadFn load = whisper_ggml_load_slot())
        load();
}
#endif

std::string trim(const std::string& s)
{
    size_t b = 0, e = s.size();
    while (b < e && std::isspace(static_cast<unsigned char>(s[b]))) ++b;
    while (e > b && std::isspace(static_cast<unsigned char>(s[e - 1]))) --e;
    return s.substr(b, e - b);
}

} // namespace

std::string whisper_local_transcribe(const std::string& wav_path,
                                     const std::string& model_path,
                                     const std::string& language,
                                     int                n_threads)
{
    WhisperLocalOptions options;
    options.language = language;
    options.n_threads = n_threads;
    return whisper_local_transcribe_with_options(wav_path, model_path, options);
}

int flash_code(WhisperFlashAttention flash)
{
#if !defined(PM_AUDIO_LINK_WHISPER)
    if (flash == WhisperFlashAttention::On)
        return PM_WHISPER_FLASH_ON;
    if (flash == WhisperFlashAttention::Off)
        return PM_WHISPER_FLASH_OFF;
    return PM_WHISPER_FLASH_AUTO;
#else
    (void)flash;
    return 0;
#endif
}

void ensure_whisper_allowed(const std::string& model_path)
{
    if (WhisperGateFn gate = whisper_gate_slot()) {
        std::string err;
        if (!gate(err)) {
            throw std::runtime_error(err.empty()
                ? std::string("whisper_local: local whisper.cpp inference is disabled by Group Policy "
                              "(Local.EnableWhisper = 0).")
                : err);
        }
    }
    if (!path_exists(model_path)) {
        const WhisperModelProbe probe = whisper_probe_model_path(model_path);
        pm::audio::log::trace("[whisper] transcribe model missing path=" + model_path);
        throw std::runtime_error("whisper_local: " + whisper_model_not_found_message(probe));
    }
}

// Application jobs for long files (memory / cancel / progress). Whisper's own
// encoder already walks ~30 s windows — do not slice at that grain, and do not
// shrink audio_ctx. Direct whisper_full below this; overlapping jobs above.
constexpr std::size_t kWhisperRate = 16000;
constexpr std::size_t kWhisperDirectMax = 30ull * 60ull * kWhisperRate; // 30 min
constexpr std::size_t kWhisperJob      = 10ull * 60ull * kWhisperRate; // 10 min
constexpr std::size_t kWhisperOverlap  = 2ull * kWhisperRate;          // 2 s

std::int64_t samples_to_cs(std::size_t samples) noexcept
{
    return static_cast<std::int64_t>((samples * 100ull) / kWhisperRate);
}

std::string norm_seg_text(std::string s)
{
    for (char& c : s) {
        if (c >= 'A' && c <= 'Z')
            c = static_cast<char>(c - 'A' + 'a');
        if (c == '\n' || c == '\r' || c == '\t')
            c = ' ';
    }
    std::string o;
    o.reserve(s.size());
    bool sp = false;
    for (char c : s) {
        if (c == ' ') {
            if (!o.empty())
                sp = true;
            continue;
        }
        if (sp) {
            o.push_back(' ');
            sp = false;
        }
        o.push_back(c);
    }
    return o;
}

bool segments_near_dup(const WhisperSegment& a, const WhisperSegment& b)
{
    if (a.t1_cs < b.t0_cs || b.t1_cs < a.t0_cs)
        return false;
    const auto na = norm_seg_text(a.text);
    const auto nb = norm_seg_text(b.text);
    if (na.empty() || nb.empty())
        return false;
    if (na == nb)
        return true;
    return na.find(nb) != std::string::npos || nb.find(na) != std::string::npos;
}

void append_whisper_job(WhisperTranscript& acc,
                        WhisperTranscript job,
                        std::int64_t offset_cs,
                        std::int64_t overlap_cs,
                        bool first)
{
    for (auto& s : job.segments) {
        s.t0_cs += offset_cs;
        s.t1_cs += offset_cs;
        if (s.t1_cs < s.t0_cs)
            s.t1_cs = s.t0_cs;
        for (auto& w : s.words) {
            w.t0_cs += offset_cs;
            w.t1_cs += offset_cs;
            if (w.t1_cs < w.t0_cs)
                w.t1_cs = w.t0_cs;
        }
        if (!first && s.t1_cs <= offset_cs + overlap_cs)
            continue;
        if (!acc.segments.empty() && segments_near_dup(acc.segments.back(), s))
            continue;
        acc.segments.push_back(std::move(s));
    }
}

void rebuild_whisper_text(WhisperTranscript& t)
{
    t.text.clear();
    for (const auto& s : t.segments)
        t.text += s.text;
    // trim later at call site if needed
}

static std::string collapse_ws(const std::string& in)
{
    std::string o;
    bool sp = false;
    for (unsigned char c : in) {
        if (std::isspace(c)) {
            if (!o.empty())
                sp = true;
            continue;
        }
        if (sp) {
            o.push_back(' ');
            sp = false;
        }
        o.push_back(static_cast<char>(c));
    }
    return o;
}

static std::vector<std::string> split_ws(const std::string& s)
{
    std::vector<std::string> out;
    std::string cur;
    for (unsigned char c : s) {
        if (std::isspace(c)) {
            if (!cur.empty()) {
                out.push_back(std::move(cur));
                cur.clear();
            }
        } else {
            cur.push_back(static_cast<char>(c));
        }
    }
    if (!cur.empty())
        out.push_back(std::move(cur));
    return out;
}

static void borrow_collapsed_clocks(WhisperSegment& s)
{
    size_t i = 0;
    while (i < s.words.size()) {
        const std::int64_t prev_end = (i == 0)
            ? std::max<std::int64_t>(s.t0_cs, 0)
            : s.words[i - 1].t1_cs;
        auto& w = s.words[i];
        const bool zero_at_origin = w.t0_cs == 0 && w.t1_cs <= 0 && prev_end > 0;
        const bool bad_start = w.t0_cs < 0 || w.t0_cs < prev_end || zero_at_origin;
        if (!bad_start && w.t1_cs > w.t0_cs) {
            ++i;
            continue;
        }
        if (!bad_start) {
            w.t1_cs = w.t0_cs + 1;
            ++i;
            continue;
        }
        size_t j = i + 1;
        while (j < s.words.size()) {
            const auto& n = s.words[j];
            const bool n_origin = n.t0_cs == 0 && n.t1_cs <= 0;
            if (n.t0_cs >= 0 && n.t1_cs > n.t0_cs && !n_origin)
                break;
            ++j;
        }
        std::int64_t left = std::max<std::int64_t>(prev_end, 0);
        std::int64_t right = (j < s.words.size()) ? s.words[j].t0_cs : s.t1_cs;
        const auto nrun = static_cast<std::int64_t>(j - i);
        if (right <= left)
            right = left + nrun;
        const auto span = right - left;
        for (size_t k = i; k < j; ++k) {
            const auto idx = static_cast<std::int64_t>(k - i);
            auto t0 = left + span * idx / nrun;
            auto t1 = left + span * (idx + 1) / nrun;
            if (t1 <= t0)
                t1 = t0 + 1;
            s.words[k].t0_cs = t0;
            s.words[k].t1_cs = t1;
        }
        i = j;
    }
}

static void finish_whisper_segment(WhisperSegment& s)
{
    if (s.words.empty())
        return;
    std::string joined;
    for (const auto& w : s.words) {
        const auto t = collapse_ws(w.text);
        if (t.empty())
            continue;
        if (!joined.empty())
            joined.push_back(' ');
        joined += t;
    }
    const auto seg = collapse_ws(s.text);
    const auto seg_tokens = split_ws(seg);
    if (joined != seg) {
        std::vector<WhisperWord> rebuilt;
        rebuilt.reserve(seg_tokens.size() + s.words.size());
        std::string missing;
        size_t j = 0;
        for (size_t i = 0; i < seg_tokens.size(); ++i) {
            if (j < s.words.size() && collapse_ws(s.words[j].text) == seg_tokens[i]) {
                rebuilt.push_back(s.words[j]);
                ++j;
                continue;
            }
            bool later = false;
            if (j < s.words.size()) {
                for (size_t k = i + 1; k < seg_tokens.size(); ++k) {
                    if (collapse_ws(s.words[j].text) == seg_tokens[k]) {
                        later = true;
                        break;
                    }
                }
            }
            if (later || j >= s.words.size()) {
                WhisperWord w;
                w.text = seg_tokens[i];
                w.t0_cs = -1;
                w.t1_cs = -1;
                rebuilt.push_back(std::move(w));
                if (!missing.empty())
                    missing.push_back(' ');
                missing += seg_tokens[i];
                continue;
            }
            rebuilt.push_back(s.words[j]);
            ++j;
        }
        while (j < s.words.size()) {
            rebuilt.push_back(std::move(s.words[j]));
            ++j;
        }
        s.words = std::move(rebuilt);
        s.word_map_note = "WORD MAP MISMATCH\nsegment: \"" + seg
            + "\"\nwords:   \"" + joined + "\"\nmissing: \"" + missing + "\"";
        pm::audio::log::warn("[whisper] " + s.word_map_note);
    }
    borrow_collapsed_clocks(s);
}

WhisperTranscript parse_whisper_segments_json(const std::string& json,
                                              const std::string& text_fallback,
                                              std::size_t n_samples)
{
    WhisperTranscript out;
    if (!json.empty() && json != "[]") {
        try {
            const auto j = nlohmann::json::parse(json);
            if (j.is_array()) {
                for (const auto& e : j) {
                    if (!e.is_object())
                        continue;
                    WhisperSegment s;
                    s.t0_cs = e.value("t0", 0);
                    s.t1_cs = e.value("t1", 0);
                    s.text = e.value("text", std::string{});
                    if (e.value("dtw", 0) != 0)
                        out.dtw = true;
                    if (e.contains("words") && e["words"].is_array()) {
                        for (const auto& wj : e["words"]) {
                            if (!wj.is_object())
                                continue;
                            WhisperWord w;
                            w.t0_cs = wj.value("t0", static_cast<std::int64_t>(0));
                            w.t1_cs = wj.value("t1", static_cast<std::int64_t>(0));
                            w.text = wj.value("text", std::string{});
                            if (w.text.empty())
                                continue;
                            s.words.push_back(std::move(w));
                        }
                    }
                    finish_whisper_segment(s);
                    if (s.text.empty() && s.words.empty())
                        continue;
                    out.segments.push_back(std::move(s));
                    out.text += out.segments.back().text;
                }
            }
        } catch (...) {
            out.segments.clear();
            out.text.clear();
        }
    }
    if (out.segments.empty() && !text_fallback.empty()) {
        WhisperSegment s;
        s.text = text_fallback;
        s.t1_cs = samples_to_cs(n_samples);
        out.segments.push_back(std::move(s));
        out.text = text_fallback;
    }
    return out;
}

bool whisper_cache_snapshot(std::string& path, bool& use_gpu, int& gpu_device);

#if !defined(PM_AUDIO_LINK_WHISPER)
const WhisperBackend* whisper_plugin_api()
{
    static thread_local WhisperBackend stored;
    stored = {};
    WhisperResolveFn resolve = whisper_resolve_slot();
    if (!resolve || !resolve(stored) || !stored.transcribe)
        return nullptr;
    return &stored;
}
#else
struct HostCachedCtx {
    std::mutex mu;
    std::string path;
    bool use_gpu = false;
    int gpu_device = 0;
    WhisperFlashAttention flash = WhisperFlashAttention::Auto;
    bool word_timestamps = true;
    whisper_context* ctx = nullptr;

    void reset_locked()
    {
        if (ctx) {
            whisper_free(ctx);
            ctx = nullptr;
        }
        path.clear();
    }
};

HostCachedCtx& host_whisper_cache()
{
    static HostCachedCtx cache;
    return cache;
}

whisper_context* host_load_ctx_locked(const std::string& model_path,
                                      const WhisperLocalOptions& options)
{
    auto& cache = host_whisper_cache();
    const bool want_gpu = options.use_gpu
        && !g_whisper_gpu_unavailable.load(std::memory_order_relaxed);
    if (cache.ctx && cache.path == model_path && cache.use_gpu == want_gpu &&
        cache.gpu_device == options.gpu_device && cache.flash == options.flash_attn &&
        cache.word_timestamps == options.word_timestamps)
        return cache.ctx;

    cache.reset_locked();
    install_whisper_log_once();
    load_ggml_backends_once();

    whisper_context_params cparams = whisper_context_default_params();
    cparams.gpu_device = options.gpu_device;
    // whisper.cpp turns DTW off when flash attention is on.
    cparams.flash_attn = false;
    cparams.dtw_token_timestamps = options.word_timestamps;
    cparams.dtw_aheads_preset = WHISPER_AHEADS_LARGE_V3_TURBO;
    if (options.word_timestamps) {
        std::string name = model_path;
        for (char& c : name) {
            if (c >= 'A' && c <= 'Z')
                c = static_cast<char>(c - 'A' + 'a');
            if (c == '_')
                c = '-';
        }
        auto has = [&](const char* n) { return name.find(n) != std::string::npos; };
        if (has("large-v3-turbo") || has("large-v3.turbo"))
            cparams.dtw_aheads_preset = WHISPER_AHEADS_LARGE_V3_TURBO;
        else if (has("large-v3"))
            cparams.dtw_aheads_preset = WHISPER_AHEADS_LARGE_V3;
        else if (has("large-v2"))
            cparams.dtw_aheads_preset = WHISPER_AHEADS_LARGE_V2;
        else if (has("large"))
            cparams.dtw_aheads_preset = WHISPER_AHEADS_LARGE_V1;
        else if (has("medium.en") || has("medium-en"))
            cparams.dtw_aheads_preset = WHISPER_AHEADS_MEDIUM_EN;
        else if (has("medium"))
            cparams.dtw_aheads_preset = WHISPER_AHEADS_MEDIUM;
        else if (has("small.en") || has("small-en"))
            cparams.dtw_aheads_preset = WHISPER_AHEADS_SMALL_EN;
        else if (has("small"))
            cparams.dtw_aheads_preset = WHISPER_AHEADS_SMALL;
        else if (has("base.en") || has("base-en"))
            cparams.dtw_aheads_preset = WHISPER_AHEADS_BASE_EN;
        else if (has("base"))
            cparams.dtw_aheads_preset = WHISPER_AHEADS_BASE;
        else if (has("tiny.en") || has("tiny-en"))
            cparams.dtw_aheads_preset = WHISPER_AHEADS_TINY_EN;
        else if (has("tiny"))
            cparams.dtw_aheads_preset = WHISPER_AHEADS_TINY;
    }
    pm::audio::log::info("[whisper] dtw=" + std::string(options.word_timestamps ? "1" : "0")
                 + " flash=0 preset="
                 + std::to_string(static_cast<int>(cparams.dtw_aheads_preset)));

    if (want_gpu) {
        cparams.use_gpu = true;
        pm::audio::log::debug("[whisper] load model=" + model_path + " gpu=1");
        whisper_context* ctx = whisper_init_from_file_with_params(model_path.c_str(), cparams);
        if (ctx) {
            cache.ctx = ctx;
            cache.path = model_path;
            cache.use_gpu = true;
            cache.gpu_device = options.gpu_device;
            cache.flash = options.flash_attn;
            cache.word_timestamps = options.word_timestamps;
            return ctx;
        }
        note_whisper_gpu_fallback("whisper_init_from_file_with_params");
        cache.reset_locked();
    }

    cparams.use_gpu = false;
    pm::audio::log::debug("[whisper] load model=" + model_path + " gpu=0");
    whisper_context* ctx = whisper_init_from_file_with_params(model_path.c_str(), cparams);
    if (!ctx)
        throw std::runtime_error("whisper_local: failed to load model: " + model_path);
    cache.ctx = ctx;
    cache.path = model_path;
    cache.use_gpu = false;
    cache.gpu_device = options.gpu_device;
    cache.flash = options.flash_attn;
    cache.word_timestamps = options.word_timestamps;
    return ctx;
}

void fill_whisper_words(whisper_context* ctx, int seg_index, WhisperSegment& seg)
{
    seg.words.clear();
    if (!ctx)
        return;
    const whisper_token eot = whisper_token_eot(ctx);
    const int n = whisper_full_n_tokens(ctx, seg_index);
    WhisperWord cur;
    auto flush = [&]() {
        cur.text = trim(cur.text);
        if (!cur.text.empty())
            seg.words.push_back(std::move(cur));
        cur = {};
    };
    for (int j = 0; j < n; ++j) {
        const whisper_token id = whisper_full_get_token_id(ctx, seg_index, j);
        if (id >= eot)
            continue;
        const char* raw = whisper_full_get_token_text(ctx, seg_index, j);
        if (!raw || !raw[0])
            continue;
        const whisper_token_data td = whisper_full_get_token_data(ctx, seg_index, j);
        const std::int64_t span0 = td.t_dtw >= 0 ? td.t_dtw : td.t0;
        const std::int64_t span1 = td.t_dtw >= 0 ? td.t_dtw : td.t1;
        const bool boundary = raw[0] == ' ' || cur.text.empty();
        if (boundary && !cur.text.empty())
            flush();
        if (cur.text.empty()) {
            cur.t0_cs = span0;
            cur.t1_cs = span1 > span0 ? span1 : span0;
            if (*raw == ' ')
                ++raw;
        } else if (span1 > cur.t1_cs) {
            cur.t1_cs = span1;
        }
        cur.text += raw;
    }
    flush();
}
#endif

std::string whisper_local_transcribe_pcm(const float* pcm_16k,
                                         std::size_t n_samples,
                                         const std::string& model_path,
                                         const WhisperLocalOptions& options,
                                         const std::atomic_bool* cancel)
{
    return whisper_local_transcribe_pcm_detailed(
        pcm_16k, n_samples, model_path, options, cancel).text;
}

WhisperTranscript whisper_local_transcribe_pcm_detailed(
    const float* pcm_16k,
    std::size_t n_samples,
    const std::string& model_path,
    const WhisperLocalOptions& options,
    const std::atomic_bool* cancel)
{
    ensure_whisper_allowed(model_path);
    if (!pcm_16k || n_samples == 0)
        throw std::runtime_error("whisper_local: empty PCM");
    if (n_samples > static_cast<std::size_t>(std::numeric_limits<int>::max()))
        throw std::runtime_error("whisper_local: PCM exceeds whisper_full length");

    WhisperLocalOptions opts = coerce_whisper_gpu(options, false);
    if (whisper_full_should_abort(const_cast<std::atomic_bool*>(cancel)))
        throw std::runtime_error("whisper_local: cancelled");

    int threads = opts.n_threads;
    if (threads <= 0) {
        const unsigned hw = std::thread::hardware_concurrency();
        threads = static_cast<int>(hw == 0 ? 4u : std::min(hw, 8u));
    }
    const std::string lang_in = opts.language.empty() ? std::string("auto") : opts.language;
    std::string lang = whisper_normalize_language(lang_in);
    if (lang != lang_in)
        pm::audio::log::info("[whisper] language \"" + lang_in + "\" → " + lang);

    std::string cached_path;
    bool cached_gpu = false;
    int cached_device = 0;
    const bool reused = whisper_cache_snapshot(cached_path, cached_gpu, cached_device)
        && cached_path == model_path
        && cached_gpu == opts.use_gpu
        && cached_device == opts.gpu_device;
    const auto stt_begin = std::chrono::steady_clock::now();

    ActiveWhisperScope active(ActiveWhisperModel{
        next_whisper_active_id(),
        model_path,
        lang,
        opts.use_gpu,
        opts.gpu_device,
        threads,
        now_unix_ms(),
    });

    auto run_window = [&](const float* pcm, std::size_t n) -> WhisperTranscript {
        if (whisper_full_should_abort(const_cast<std::atomic_bool*>(cancel)))
            throw std::runtime_error("whisper_local: cancelled");
#if !defined(PM_AUDIO_LINK_WHISPER)
        const auto* api = whisper_plugin_api();
        if (!api || !api->transcribe)
            throw std::runtime_error("whisper_local: audio.whisper is unavailable");
        auto run_plugin = [&](bool use_gpu) -> WhisperTranscript {
            char* text_out = nullptr;
            char* segs_json = nullptr;
            char err[512] = {};
            int rc = 1;
            const char* prompt = opts.initial_prompt.empty()
                ? nullptr : opts.initial_prompt.c_str();
            if (api->abi_version >= PM_WHISPER_TASK_API && api->transcribe_segments) {
                rc = api->transcribe_segments(
                    pcm, static_cast<uint32_t>(n), model_path.c_str(), lang.c_str(),
                    threads, use_gpu ? 1 : 0, opts.gpu_device,
                    flash_code(opts.flash_attn), prompt,
                    &segs_json, &text_out, err, sizeof(err));
            } else {
                rc = api->transcribe(
                    pcm, static_cast<uint32_t>(n), model_path.c_str(), lang.c_str(),
                    threads, use_gpu ? 1 : 0, opts.gpu_device,
                    flash_code(opts.flash_attn), prompt,
                    &text_out, err, sizeof(err));
            }
            std::string text = text_out ? text_out : "";
            std::string json = segs_json ? segs_json : "";
            if (api->free_result) {
                if (text_out)
                    api->free_result(text_out);
                if (segs_json)
                    api->free_result(segs_json);
            }
            if (rc != 0)
                throw std::runtime_error(std::string("whisper_local: ")
                                         + (err[0] ? err : "inference failed"));
            auto t = parse_whisper_segments_json(json, trim(text), n);
            t.text = trim(t.text);
            if (t.text == "[BLANK_AUDIO]" || t.text == "[NO SPEECH]"
                || t.text == "[no speech]") {
                t.text.clear();
                t.segments.clear();
            }
            return t;
        };
        try {
            return run_plugin(opts.use_gpu);
        } catch (const std::exception& ex) {
            if (!opts.use_gpu)
                throw;
            note_whisper_gpu_fallback(ex.what());
            opts.use_gpu = false;
            return run_plugin(false);
        }
#else
        auto& cache = host_whisper_cache();
        std::lock_guard<std::mutex> lock(cache.mu);
        whisper_context* ctx = host_load_ctx_locked(model_path, opts);

        whisper_full_params wparams = whisper_full_default_params(WHISPER_SAMPLING_GREEDY);
        wparams.n_threads        = threads;
        wparams.translate        = false;
        // Timestamp tokens must be allowed to split. single_segment glues a
        // pause into one cue that stays up until the next phrase.
        wparams.single_segment   = false;
        wparams.no_context       = n <= kWhisperRate * 6u;
        wparams.token_timestamps = opts.word_timestamps;
        wparams.print_progress   = false;
        wparams.print_realtime   = false;
        wparams.print_timestamps = false;
        wparams.print_special    = false;
        const bool english_only = whisper_is_multilingual(ctx) == 0;
        std::string win_lang = lang;
        if (english_only)
            win_lang = "en";
        wparams.language         = win_lang.c_str();
        wparams.detect_language  = false;
        if (!opts.initial_prompt.empty())
            wparams.initial_prompt = opts.initial_prompt.c_str();
        wparams.abort_callback = whisper_full_should_abort;
        wparams.abort_callback_user_data =
            const_cast<std::atomic_bool*>(cancel);

        const int rc = whisper_full(ctx, wparams, pcm, static_cast<int>(n));
        if (whisper_full_should_abort(const_cast<std::atomic_bool*>(cancel)))
            throw std::runtime_error("whisper_local: cancelled");
        if (rc != 0)
            throw std::runtime_error("whisper_local: inference failed (whisper_full="
                                     + std::to_string(rc) + ")");

        WhisperTranscript t;
        const int n_seg = whisper_full_n_segments(ctx);
        for (int i = 0; i < n_seg; ++i) {
            const char* seg = whisper_full_get_segment_text(ctx, i);
            if (!seg)
                continue;
            WhisperSegment s;
            s.text = seg;
            s.t0_cs = whisper_full_get_segment_t0(ctx, i);
            s.t1_cs = whisper_full_get_segment_t1(ctx, i);
            if (opts.word_timestamps)
                fill_whisper_words(ctx, i, s);
            finish_whisper_segment(s);
            t.text += s.text;
            t.segments.push_back(std::move(s));
        }
        t.dtw = opts.word_timestamps;
        t.text = trim(t.text);
        if (t.text == "[BLANK_AUDIO]" || t.text == "[NO SPEECH]"
            || t.text == "[no speech]") {
            t.text.clear();
            t.segments.clear();
        }
        return t;
#endif
    };

    WhisperTranscript out;
    if (n_samples <= kWhisperDirectMax) {
        out = run_window(pcm_16k, n_samples);
    } else {
        const std::size_t step = kWhisperJob - kWhisperOverlap;
        const int jobs = static_cast<int>(
            (n_samples + step - 1) / step);
        pm::audio::log::info("[whisper] long WAV duration_s="
                     + std::to_string(n_samples / kWhisperRate)
                     + " jobs~" + std::to_string(jobs)
                     + " job_s=600 overlap_s=2 (keep model cached)");
        whisper_local_preload(model_path, opts);
        std::size_t start = 0;
        int job_i = 0;
        while (start < n_samples) {
            const std::size_t n = std::min(kWhisperJob, n_samples - start);
            ++job_i;
            pm::audio::log::info("[whisper] job " + std::to_string(job_i)
                         + "/" + std::to_string(jobs)
                         + " start_s=" + std::to_string(start / kWhisperRate)
                         + " win_s=" + std::to_string(n / kWhisperRate));
            auto part = run_window(pcm_16k + start, n);
            append_whisper_job(out, std::move(part), samples_to_cs(start),
                               samples_to_cs(kWhisperOverlap), start == 0);
            if (n < kWhisperJob || start + n >= n_samples)
                break;
            start += step;
        }
        rebuild_whisper_text(out);
        out.text = trim(out.text);
    }

    {
        const auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(
            std::chrono::steady_clock::now() - stt_begin).count();
        pm::audio::log::debug("[whisper] stt ms=" + std::to_string(ms)
                     + " cached=" + (reused ? "1" : "0")
                     + " gpu=" + std::string(opts.use_gpu ? "1" : "0")
                     + " pcm=" + std::to_string(n_samples)
                     + " segments=" + std::to_string(out.segments.size())
                     + " model=" + pm::audio::path_to_utf8(pm::audio::path_from_utf8(model_path).filename()));
    }
    return out;
}

void whisper_local_preload(const std::string& model_path,
                           const WhisperLocalOptions& options)
{
    ensure_whisper_allowed(model_path);
    auto try_preload = [&](const WhisperLocalOptions& opts) {
        std::string cached_path;
        bool cached_gpu = false;
        int cached_device = 0;
        if (whisper_cache_snapshot(cached_path, cached_gpu, cached_device)
            && cached_path == model_path
            && cached_gpu == opts.use_gpu
            && cached_device == opts.gpu_device) {
            pm::audio::log::debug("[whisper] preload reuse model="
                         + pm::audio::path_to_utf8(pm::audio::path_from_utf8(model_path).filename())
                         + " gpu=" + std::string(opts.use_gpu ? "1" : "0"));
        } else {
            pm::audio::log::debug("[whisper] preload load model="
                         + pm::audio::path_to_utf8(pm::audio::path_from_utf8(model_path).filename())
                         + " gpu=" + std::string(opts.use_gpu ? "1" : "0"));
        }
#if !defined(PM_AUDIO_LINK_WHISPER)
        const auto* api = whisper_plugin_api();
        if (!api)
            throw std::runtime_error("whisper_local: audio.whisper is unavailable");
        if (!api->preload)
            return;
        char err[512] = {};
        if (api->preload(model_path.c_str(), opts.use_gpu ? 1 : 0, opts.gpu_device,
                         flash_code(opts.flash_attn), err, sizeof(err)) != 0) {
            throw std::runtime_error(std::string("whisper_local: ") + (err[0] ? err : "preload failed"));
        }
#else
        std::lock_guard<std::mutex> lock(host_whisper_cache().mu);
        (void)host_load_ctx_locked(model_path, opts);
#endif
    };

    if (options.use_gpu) {
        try {
            try_preload(options);
            g_whisper_gpu_unavailable.store(false, std::memory_order_relaxed);
            return;
        } catch (const std::exception& ex) {
            note_whisper_gpu_fallback(ex.what());
            WhisperLocalOptions cpu = options;
            cpu.use_gpu = false;
            try_preload(cpu);
            return;
        }
    }
    try_preload(options);
}

std::string whisper_local_transcribe_with_options(const std::string& wav_path,
                                                  const std::string& model_path,
                                                  const WhisperLocalOptions& options)
{
    return whisper_local_transcribe_detailed(wav_path, model_path, options).text;
}

WhisperTranscript whisper_local_transcribe_detailed(
    const std::string& wav_path,
    const std::string& model_path,
    const WhisperLocalOptions& options)
{
    const std::vector<float> pcm = read_wav_16k(wav_path);
    if (pcm.empty()) throw std::runtime_error("whisper_local: empty audio in " + wav_path);
    return whisper_local_transcribe_pcm_detailed(
        pcm.data(), pcm.size(), model_path, options, nullptr);
}

bool whisper_cache_snapshot(std::string& path, bool& use_gpu, int& gpu_device)
{
#if !defined(PM_AUDIO_LINK_WHISPER)
    const auto* api = whisper_plugin_api();
    if (!api || !api->cache_info)
        return false;
    char path_buf[1024] = {};
    int gpu = 0;
    int device = 0;
    if (api->cache_info(path_buf, sizeof(path_buf), &gpu, &device) != 0)
        return false;
    path = path_buf;
    use_gpu = gpu != 0;
    gpu_device = device;
    return true;
#else
    auto& cache = host_whisper_cache();
    std::lock_guard<std::mutex> lock(cache.mu);
    if (!cache.ctx)
        return false;
    path = cache.path;
    use_gpu = cache.use_gpu;
    gpu_device = cache.gpu_device;
    return true;
#endif
}

nlohmann::json whisper_loaded_models_json()
{
    std::string path;
    std::string language;
    bool use_gpu = false;
    int gpu_device = 0;
    int threads = 0;
    std::int64_t started_unix_ms = 0;
    int active_calls = 0;
    {
        std::lock_guard<std::mutex> lk(active_whisper_mutex());
        active_calls = static_cast<int>(active_whisper_models().size());
        if (active_calls > 0) {
            const auto& row = active_whisper_models().front();
            path = row.model_path;
            language = row.language;
            use_gpu = row.use_gpu;
            gpu_device = row.gpu_device;
            threads = row.threads;
            started_unix_ms = row.started_unix_ms;
        }
    }
    if (active_calls == 0 && !whisper_cache_snapshot(path, use_gpu, gpu_device))
        return nlohmann::json::array();

    nlohmann::json row{
        {"key", "whisper:stt"},
        {"kind", "whisper-stt"},
        {"name", path.empty() ? "whisper" : pm::audio::path_to_utf8(pm::audio::path_from_utf8(path).filename())},
        {"path", path},
        {"status", active_calls > 0 ? "busy" : "loaded"},
        {"provider", "whisper"},
        {"backend", use_gpu ? "gpu" : "cpu"},
        {"gpuDevice", gpu_device},
        {"activeCalls", active_calls},
        {"stopSupported", true},
    };
    if (active_calls > 0) {
        row["language"] = language;
        row["threads"] = threads;
        row["startedUnixMs"] = started_unix_ms;
    }
    return nlohmann::json::array({std::move(row)});
}

bool whisper_unload_loaded_model(std::string_view key, std::string& err)
{
    if (!key.empty() && key != "whisper:stt" && key.rfind("whisper:", 0) != 0) {
        err = "not a whisper model key";
        return false;
    }
    {
        std::lock_guard<std::mutex> lk(active_whisper_mutex());
        if (!active_whisper_models().empty()) {
            err = "whisper model is busy";
            return false;
        }
    }
#if !defined(PM_AUDIO_LINK_WHISPER)
    const auto* api = whisper_plugin_api();
    if (!api || !api->unload) {
        err = "audio.whisper unload is unavailable";
        return false;
    }
    char buf[256] = {};
    if (api->unload(buf, sizeof(buf)) != 0) {
        err = buf[0] ? buf : "whisper unload failed";
        return false;
    }
    return true;
#else
    auto& cache = host_whisper_cache();
    std::unique_lock<std::mutex> lock(cache.mu, std::try_to_lock);
    if (!lock.owns_lock()) {
        err = "whisper model is busy";
        return false;
    }
    if (!cache.ctx) {
        err = "whisper model is not loaded";
        return false;
    }
    cache.reset_locked();
    return true;
#endif
}

} // namespace pm::stt

