#include "pm/audio/tts/elevenlabs_tts.hpp"
#include "pm/audio/text.hpp"
#include "pm/audio/tts/elevenlabs_voice_change.hpp"
#include "pm/audio/constants.hpp"

#include "pm/audio/log.hpp"

#include <curl/curl.h>
#include <nlohmann/json.hpp>

#include <cctype>
#include <chrono>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <memory>
#include <sstream>
#include <stdexcept>
#include <string>
#include <vector>
#include "pm/audio/text.hpp"

namespace C = pm::audio::constants;

namespace pm::tts {

// ── Persistent per-thread curl handle ─────────────────────────────────────────
// Reusing the same CURL* across requests lets libcurl reuse the underlying
// TCP + TLS session when the host matches. ElevenLabs always responds with
// keep-alive, so subsequent speaks save the ~200–400 ms handshake cost that
// dominated the first-audio latency budget.
// The unique_ptr deleter calls curl_easy_cleanup at thread exit.
namespace {
struct CurlEasyDeleter {
    void operator()(CURL* c) const noexcept { if (c) curl_easy_cleanup(c); }
};
using CurlEasyHandle = std::unique_ptr<CURL, CurlEasyDeleter>;

CURL* tls_curl_handle() {
    static thread_local CurlEasyHandle h;
    if (!h) {
        h.reset(curl_easy_init());
        if (!h) return nullptr;
        // Stable options — set once per handle.
        curl_easy_setopt(h.get(), CURLOPT_NOSIGNAL, 1L);
        // Force HTTP/1.1; ElevenLabs supports it and we sidestep occasional
        // h2 framing oddities in some libcurl builds.
        curl_easy_setopt(h.get(), CURLOPT_HTTP_VERSION,
                         static_cast<long>(CURL_HTTP_VERSION_1_1));
        // TCP keepalive keeps the socket warm between speaks.
        curl_easy_setopt(h.get(), CURLOPT_TCP_KEEPALIVE, 1L);
        pm::audio::log::trace("[speak] elevenlabs: curl handle initialised (thread_local)");
    }
    return h.get();
}

// Reset request-scoped options so a previous call's state can't bleed in.
// CURLOPT_HTTPHEADER is replaced by the next curl_easy_setopt call, so no
// explicit reset is needed for it.
void curl_reset_per_request(CURL* c) {
    curl_easy_setopt(c, CURLOPT_NOPROGRESS,       1L);
    curl_easy_setopt(c, CURLOPT_XFERINFOFUNCTION, nullptr);
    curl_easy_setopt(c, CURLOPT_XFERINFODATA,     nullptr);
    curl_easy_setopt(c, CURLOPT_POSTFIELDS,       nullptr);
    curl_easy_setopt(c, CURLOPT_POSTFIELDSIZE,    0L);
    curl_easy_setopt(c, CURLOPT_WRITEFUNCTION,    nullptr);
    curl_easy_setopt(c, CURLOPT_WRITEDATA,        nullptr);
    curl_easy_setopt(c, CURLOPT_HEADERFUNCTION,   nullptr);
    curl_easy_setopt(c, CURLOPT_HEADERDATA,       nullptr);
}
} // namespace

// ── helpers ───────────────────────────────────────────────────────────────────

std::string tts_format_for_ext(const std::string& ext) {
    if (ext == ".wav")  return "pcm_44100";
    if (ext == ".mp3")  return "mp3_44100_128";
    if (ext == ".opus") return "opus_48000_32";
    return "mp3_44100_128";
}

bool tts_format_is_pcm(const std::string& fmt) {
    return fmt == "pcm" || (fmt.size() >= 4 && fmt.substr(0, 4) == "pcm_");
}

int tts_pcm_sample_rate(const std::string& fmt) {
    // "pcm_44100" → 44100. Bare "pcm" is the OpenRouter/xAI default: 24 kHz s16le.
    const auto pos = fmt.find('_');
    if (pos == std::string::npos) return 24000;
    try { return std::stoi(fmt.substr(pos + 1)); } catch (...) { return 24000; }
}

std::string elevenlabs_voice_change_format_for_ext(const std::string& ext) {
    // 44.1 kHz WAV requires ElevenLabs Pro; 22.05 kHz is the broadly
    // available lossless-container default. Users can still request wav_44100.
    if (ext == ".wav")  return "wav_22050";
    if (ext == ".opus") return "opus_48000_128";
    return "mp3_44100_128";
}

// ── libcurl write callback ─────────────────────────────────────────────────

static size_t curl_write_bytes(void* ptr, size_t size, size_t nmemb, void* userdata) {
    const size_t bytes = size * nmemb;
    auto* buf = static_cast<std::vector<uint8_t>*>(userdata);
    const auto* p = static_cast<const uint8_t*>(ptr);
    buf->insert(buf->end(), p, p + bytes);
    return bytes;
}

struct BufferedTtsTrace {
    std::vector<uint8_t>* bytes = nullptr;
    std::chrono::steady_clock::time_point started = std::chrono::steady_clock::now();
    std::chrono::steady_clock::time_point last_log = started;
    std::chrono::steady_clock::time_point last_chunk = started;
    size_t bytes_at_last_log = 0;
    size_t chunks = 0;
    size_t largest_chunk = 0;
    long long max_chunk_gap_ms = 0;
};

static size_t curl_write_tts_bytes(void* ptr, size_t size, size_t nmemb, void* userdata) {
    const size_t bytes = size * nmemb;
    auto* trace = static_cast<BufferedTtsTrace*>(userdata);
    const auto now = std::chrono::steady_clock::now();
    const auto gap_ms = std::chrono::duration_cast<std::chrono::milliseconds>(
        now - trace->last_chunk).count();
    if (gap_ms > trace->max_chunk_gap_ms) trace->max_chunk_gap_ms = gap_ms;
    trace->last_chunk = now;
    ++trace->chunks;
    if (bytes > trace->largest_chunk) trace->largest_chunk = bytes;

    const auto* p = static_cast<const uint8_t*>(ptr);
    trace->bytes->insert(trace->bytes->end(), p, p + bytes);

    if (now - trace->last_log >= std::chrono::seconds(3)) {
        const size_t total = trace->bytes->size();
        pm::audio::log::trace("[speak] elevenlabs buffered: received=" + std::to_string(total) + "B"
                      + " delta_3s=" + std::to_string(total - trace->bytes_at_last_log) + "B"
                      + " chunks=" + std::to_string(trace->chunks)
                      + " largest_chunk=" + std::to_string(trace->largest_chunk) + "B"
                      + " max_chunk_gap=" + std::to_string(trace->max_chunk_gap_ms) + "ms");
        trace->bytes_at_last_log = total;
        trace->last_log = now;
    }
    return bytes;
}

static size_t curl_write_ring(void* ptr, size_t size, size_t nmemb, void* userdata) {
    auto* ring = static_cast<pm::audio::AudioRingBuffer*>(userdata);
    const size_t total = size * nmemb;
    if (!ring->push(ptr, total)) return 0; // cancelled → CURLE_WRITE_ERROR
    return total;
}

struct StreamRingWriteCtx {
    pm::audio::AudioRingBuffer* ring;
    std::vector<uint8_t>        error_buf;
    std::string                 request_id;
    bool                        http_ok = true;
};

static size_t curl_header_stream(char* buffer, size_t size, size_t nitems, void* userdata) {
    auto* ctx = static_cast<StreamRingWriteCtx*>(userdata);
    const size_t total = size * nitems;
    if (total >= 12 && std::strncmp(buffer, "HTTP/", 5) == 0) {
        const char* code_start = std::strchr(buffer, ' ');
        if (code_start) {
            const int code = std::atoi(code_start + 1);
            ctx->http_ok = (code >= 200 && code < 300);
        }
    }
    constexpr size_t k_prefix_len = 11; // "request-id:"
    if (total > k_prefix_len) {
        std::string prefix(buffer, k_prefix_len);
        prefix = pm::audio::lower_ascii(std::move(prefix));
        if (prefix == "request-id:") {
            std::string value(buffer + k_prefix_len, total - k_prefix_len);
            const auto b = value.find_first_not_of(" \t\r\n");
            const auto e = value.find_last_not_of(" \t\r\n");
            if (b != std::string::npos)
                ctx->request_id = value.substr(b, e - b + 1);
        }
    }
    return total;
}

static size_t curl_write_ring_guarded(void* ptr, size_t size, size_t nmemb, void* userdata) {
    auto* ctx = static_cast<StreamRingWriteCtx*>(userdata);
    const size_t total = size * nmemb;
    if (!ctx->http_ok) {
        const auto* p = static_cast<const uint8_t*>(ptr);
        ctx->error_buf.insert(ctx->error_buf.end(), p, p + total);
        return total;
    }
    if (!ctx->ring->push(ptr, total)) return 0;
    return total;
}

// ── ElevenLabs long-text helpers ─────────────────────────────────────────────
namespace {

// Capture the `request-id` response header (needed for request stitching).
size_t curl_capture_request_id(char* buffer, size_t size, size_t nitems, void* userdata) {
    const size_t total = size * nitems;
    auto* out = static_cast<std::string*>(userdata);
    constexpr size_t k_prefix_len = 11; // "request-id:"
    if (total > k_prefix_len) {
        std::string prefix(buffer, k_prefix_len);
        prefix = pm::audio::lower_ascii(std::move(prefix));
        if (prefix == "request-id:") {
            std::string value(buffer + k_prefix_len, total - k_prefix_len);
            const auto b = value.find_first_not_of(" \t\r\n");
            const auto e = value.find_last_not_of(" \t\r\n");
            if (b != std::string::npos)
                *out = value.substr(b, e - b + 1);
        }
    }
    return total;
}

// Split text at sentence boundaries, greedy-merging sentences into chunks of
// ~chunk_target_chars. A single over-long sentence is hard-split at
// chunk_max_chars (preferring a space). Returns {text} when chunking is
// disabled or the text is already short enough.
std::vector<std::string> split_text_for_tts(const std::string& text,
                                              const ElevenLabsTTSConfig& cfg) {
    if (!cfg.chunk_sentences || text.size() <= cfg.chunk_max_chars)
        return {text};

    std::vector<std::string> sentences;
    size_t start = 0;
    for (size_t i = 0; i < text.size(); ++i) {
        const char ch = text[i];
        if (ch == '.' || ch == '!' || ch == '?' || ch == '\n') {
            size_t end = i + 1;
            while (end < text.size()
                   && (text[end] == '.' || text[end] == '!' || text[end] == '?'))
                ++end;
            while (end < text.size()
                   && std::isspace(static_cast<unsigned char>(text[end])))
                ++end;
            sentences.push_back(text.substr(start, end - start));
            start = end;
            i = end - 1;
        }
    }
    if (start < text.size())
        sentences.push_back(text.substr(start));

    std::vector<std::string> chunks;
    std::string current;
    auto flush_current = [&] {
        if (!current.empty()) {
            chunks.push_back(std::move(current));
            current.clear();
        }
    };
    for (auto& sentence : sentences) {
        // Hard-split a single sentence that exceeds the max on its own.
        while (sentence.size() > cfg.chunk_max_chars) {
            flush_current();
            size_t cut = sentence.rfind(' ', cfg.chunk_max_chars);
            if (cut == std::string::npos || cut == 0)
                cut = cfg.chunk_max_chars;
            chunks.push_back(sentence.substr(0, cut));
            sentence.erase(0, cut);
        }
        if (!current.empty()
            && current.size() + sentence.size() > cfg.chunk_target_chars)
            flush_current();
        current += sentence;
    }
    flush_current();
    return chunks;
}

// Request body for one chunk, including trial voice_settings and stitching
// context. previous_text / next_text are the neighbouring chunks (empty at
// the edges); previous_request_ids are the ids of up-to-3 preceding chunks.
std::string build_tts_body(const ElevenLabsTTSConfig& cfg,
                           const std::string& chunk_text,
                           const std::string& previous_text,
                           const std::string& next_text,
                           const std::vector<std::string>& previous_request_ids,
                           const std::vector<std::string>& next_request_ids) {
    nlohmann::json body;
    body["text"]     = chunk_text;
    body["model_id"] = cfg.model_id;
    double speed = cfg.speed;
    if (speed < 0.7) speed = 0.7;
    if (speed > 1.2) speed = 1.2;
    const bool send_speed = (speed != 1.0);
    if (cfg.send_voice_settings || send_speed) {
        nlohmann::json vs = nlohmann::json::object();
        if (cfg.send_voice_settings) {
            vs["stability"]          = cfg.stability;
            vs["similarity_boost"]   = cfg.similarity_boost;
            vs["style"]              = cfg.style;
            vs["use_speaker_boost"]  = cfg.use_speaker_boost;
        }
        if (send_speed)
            vs["speed"] = speed;
        body["voice_settings"] = std::move(vs);
    }
    if (cfg.seed >= 0)
        body["seed"] = cfg.seed;
    if (!cfg.apply_text_normalization.empty())
        body["apply_text_normalization"] = cfg.apply_text_normalization;
    // IDs and text are not cumulative: if IDs are present, omit the text twin.
    if (!previous_request_ids.empty())
        body["previous_request_ids"] = previous_request_ids;
    else if (!previous_text.empty())
        body["previous_text"] = previous_text;
    if (!next_request_ids.empty())
        body["next_request_ids"] = next_request_ids;
    else if (!next_text.empty())
        body["next_text"] = next_text;
    return body.dump();
}

void remember_request_id(std::vector<std::string>& ids, std::string id, std::size_t max_ids) {
    if (id.empty()) return;
    ids.push_back(std::move(id));
    if (ids.size() > max_ids)
        ids.erase(ids.begin());
}

} // namespace

void apply_synthesis_options(ElevenLabsTTSConfig& cfg, const TtsSynthesisOptions& opts)
{
    cfg.send_voice_settings  = opts.send_voice_settings;
    cfg.stability            = opts.stability;
    cfg.similarity_boost     = opts.similarity_boost;
    cfg.style                = opts.style;
    cfg.chunk_sentences      = opts.chunk_sentences;
    cfg.chunk_target_chars   = opts.chunk_target_chars;
    cfg.chunk_max_chars      = opts.chunk_max_chars;
    cfg.max_prev_request_ids = opts.max_prev_request_ids;
    cfg.speed                = opts.speed;
    cfg.seed                 = opts.seed;
    cfg.use_speaker_boost    = opts.use_speaker_boost;
    cfg.apply_text_normalization = opts.apply_text_normalization;
}

std::vector<uint8_t> elevenlabs_voice_change(
    const std::string& source_audio_path,
    const ElevenLabsVoiceChangeConfig& cfg)
{
    namespace fs = std::filesystem;

    if (cfg.api_key.empty())
        throw std::invalid_argument("elevenlabs_voice_change: API key is required");
    if (cfg.voice_id.empty())
        throw std::invalid_argument("elevenlabs_voice_change: voice_id is required");
    if (cfg.base_url.empty())
        throw std::invalid_argument("elevenlabs_voice_change: base_url is required");
    if (cfg.model_id.empty())
        throw std::invalid_argument("elevenlabs_voice_change: model_id is required");
    if (cfg.output_format.empty())
        throw std::invalid_argument("elevenlabs_voice_change: output_format is required");
    if (cfg.input_format != "other" && cfg.input_format != "pcm_s16le_16")
        throw std::invalid_argument(
            "elevenlabs_voice_change: input_format must be other or pcm_s16le_16");
    if (cfg.optimize_streaming_latency < 0 || cfg.optimize_streaming_latency > 4)
        throw std::invalid_argument(
            "elevenlabs_voice_change: optimize_streaming_latency must be between 0 and 4");
    if (cfg.seed < -1 || cfg.seed > 4294967295LL)
        throw std::invalid_argument(
            "elevenlabs_voice_change: seed must be between 0 and 4294967295");
    if (cfg.transfer_timeout_seconds <= 0)
        throw std::invalid_argument(
            "elevenlabs_voice_change: transfer_timeout_seconds must be positive");
    if (!cfg.voice_settings_json.empty()) {
        try {
            const auto settings = nlohmann::json::parse(cfg.voice_settings_json);
            if (!settings.is_object())
                throw std::invalid_argument(
                    "elevenlabs_voice_change: voice_settings_json must contain a JSON object");
        } catch (const nlohmann::json::exception& ex) {
            throw std::invalid_argument(
                std::string("elevenlabs_voice_change: invalid voice_settings_json: ") + ex.what());
        }
    }

    std::error_code file_ec;
    const fs::path source_path(source_audio_path);
    if (!fs::is_regular_file(source_path, file_ec) || file_ec)
        throw std::runtime_error(
            "elevenlabs_voice_change: source audio is not a readable file: " + source_audio_path);

    using CurlPtr = std::unique_ptr<CURL, CurlEasyDeleter>;
    CurlPtr curl(curl_easy_init());
    if (!curl)
        throw std::runtime_error("elevenlabs_voice_change: curl_easy_init failed");

    char* escaped_voice = curl_easy_escape(curl.get(), cfg.voice_id.c_str(),
                                           static_cast<int>(cfg.voice_id.size()));
    char* escaped_format = curl_easy_escape(curl.get(), cfg.output_format.c_str(),
                                            static_cast<int>(cfg.output_format.size()));
    if (!escaped_voice || !escaped_format) {
        if (escaped_voice) curl_free(escaped_voice);
        if (escaped_format) curl_free(escaped_format);
        throw std::runtime_error("elevenlabs_voice_change: URL encoding failed");
    }

    std::string base_url = cfg.base_url;
    while (!base_url.empty() && base_url.back() == '/')
        base_url.pop_back();
    const std::string url =
        base_url + "/v1/speech-to-speech/" + escaped_voice
        + "?output_format=" + escaped_format
        + "&optimize_streaming_latency=" + std::to_string(cfg.optimize_streaming_latency)
        + "&enable_logging=" + (cfg.enable_logging ? "true" : "false");
    curl_free(escaped_voice);
    curl_free(escaped_format);

    curl_slist* headers = nullptr;
    curl_mime* mime = nullptr;
    auto cleanup_request = [&] {
        if (mime) curl_mime_free(mime);
        if (headers) curl_slist_free_all(headers);
    };

    const std::string auth_header = "xi-api-key: " + cfg.api_key;
    headers = curl_slist_append(headers, auth_header.c_str());
    if (!headers)
        throw std::runtime_error("elevenlabs_voice_change: could not allocate request headers");

    mime = curl_mime_init(curl.get());
    if (!mime) {
        cleanup_request();
        throw std::runtime_error("elevenlabs_voice_change: could not allocate multipart body");
    }

    auto add_text_part = [&](const char* name, const std::string& value) {
        curl_mimepart* part = curl_mime_addpart(mime);
        if (!part || curl_mime_name(part, name) != CURLE_OK
            || curl_mime_data(part, value.c_str(), value.size()) != CURLE_OK)
            throw std::runtime_error(
                std::string("elevenlabs_voice_change: could not add multipart field ") + name);
    };

    try {
        curl_mimepart* audio_part = curl_mime_addpart(mime);
        if (!audio_part || curl_mime_name(audio_part, "audio") != CURLE_OK
            || curl_mime_filedata(audio_part, pm::audio::path_to_utf8(source_path).c_str()) != CURLE_OK
            || curl_mime_filename(audio_part, pm::audio::path_to_utf8(source_path.filename()).c_str()) != CURLE_OK
            || curl_mime_type(audio_part, "application/octet-stream") != CURLE_OK)
            throw std::runtime_error(
                "elevenlabs_voice_change: could not add source audio to multipart body");

        add_text_part("model_id", cfg.model_id);
        add_text_part("file_format", cfg.input_format);
        add_text_part("remove_background_noise",
                      cfg.remove_background_noise ? "true" : "false");
        if (cfg.seed >= 0)
            add_text_part("seed", std::to_string(cfg.seed));
        if (!cfg.voice_settings_json.empty())
            add_text_part("voice_settings", cfg.voice_settings_json);
    } catch (...) {
        cleanup_request();
        throw;
    }

    std::vector<uint8_t> response;
    response.reserve(C::k_elevenlabs_reserve_bytes);
    curl_easy_setopt(curl.get(), CURLOPT_URL, url.c_str());
    curl_easy_setopt(curl.get(), CURLOPT_HTTPHEADER, headers);
    curl_easy_setopt(curl.get(), CURLOPT_MIMEPOST, mime);
    curl_easy_setopt(curl.get(), CURLOPT_WRITEFUNCTION, curl_write_bytes);
    curl_easy_setopt(curl.get(), CURLOPT_WRITEDATA, &response);
    curl_easy_setopt(curl.get(), CURLOPT_NOSIGNAL, 1L);
    curl_easy_setopt(curl.get(), CURLOPT_CONNECTTIMEOUT, C::k_curl_connect_timeout_s);
    curl_easy_setopt(curl.get(), CURLOPT_TIMEOUT, cfg.transfer_timeout_seconds);

    const CURLcode result = curl_easy_perform(curl.get());
    long http_code = 0;
    curl_easy_getinfo(curl.get(), CURLINFO_RESPONSE_CODE, &http_code);
    cleanup_request();

    if (result != CURLE_OK)
        throw std::runtime_error(
            std::string("elevenlabs_voice_change: curl error: ")
            + curl_easy_strerror(result));
    if (http_code != 200) {
        const std::string body(response.begin(), response.end());
        throw std::runtime_error(
            "elevenlabs_voice_change: HTTP " + std::to_string(http_code) + ": " + body);
    }
    if (response.empty())
        throw std::runtime_error("elevenlabs_voice_change: server returned an empty response");

    return response;
}

struct StreamCancelCtx {
    pm::audio::AudioRingBuffer*           ring;
    ElevenLabsTTSConfig::ShouldCancelFn   should_cancel;
};

static int curl_stream_cancel_check(void* ud, curl_off_t, curl_off_t, curl_off_t, curl_off_t) {
    auto* ctx = static_cast<StreamCancelCtx*>(ud);
    if (ctx->ring && ctx->ring->cancelled.load(std::memory_order_acquire))
        return 1;
    if (ctx->should_cancel && ctx->should_cancel())
        return 1;
    return 0;
}

static int curl_should_cancel_check(void* ud, curl_off_t, curl_off_t, curl_off_t, curl_off_t) {
    auto* fn = static_cast<ElevenLabsTTSConfig::ShouldCancelFn*>(ud);
    return (fn && *fn && (*fn)()) ? 1 : 0;
}

static bool synthesis_cancelled(const ElevenLabsTTSConfig& cfg)
{
    return cfg.should_cancel && cfg.should_cancel();
}

static void throw_if_cancelled(const ElevenLabsTTSConfig& cfg)
{
    if (synthesis_cancelled(cfg))
        throw std::runtime_error("elevenlabs_tts: cancelled");
}

// ── synthesize ────────────────────────────────────────────────────────────────

TtsSynthResult elevenlabs_tts_synthesize_result(const std::string& text,
                                                const ElevenLabsTTSConfig& cfg)
{
    CURL* c = curl_easy_init();
    if (!c) throw std::runtime_error("elevenlabs_tts: curl_easy_init failed");

    // Build URL: /v1/text-to-speech/{voice_id}?output_format={fmt}
    const std::string url =
        "https://api.elevenlabs.io/v1/text-to-speech/" + cfg.voice_id
        + "?output_format=" + cfg.output_format;

    const std::string auth_hdr  = "xi-api-key: " + cfg.api_key;
    const std::string ct_hdr    = "Content-Type: application/json";

    curl_slist* hdrs = nullptr;
    hdrs = curl_slist_append(hdrs, auth_hdr.c_str());
    hdrs = curl_slist_append(hdrs, ct_hdr.c_str());

    const auto text_chunks = split_text_for_tts(text, cfg);
    std::vector<std::string> prev_request_ids;
    std::string last_request_id;

    std::vector<uint8_t> response;
    response.reserve(C::k_elevenlabs_reserve_bytes);
    BufferedTtsTrace trace;
    trace.bytes = &response;

    auto fail = [&](const std::string& msg) {
        curl_slist_free_all(hdrs);
        curl_easy_cleanup(c);
        throw std::runtime_error(msg);
    };

    for (size_t i = 0; i < text_chunks.size(); ++i) {
        if (synthesis_cancelled(cfg)) {
            pm::audio::log::trace("[speak] elevenlabs buffered: cancelled before chunk "
                          + std::to_string(i + 1) + "/" + std::to_string(text_chunks.size()));
            curl_slist_free_all(hdrs);
            curl_easy_cleanup(c);
            throw_if_cancelled(cfg);
        }

        std::string prev_text;
        std::string next_text;
        std::vector<std::string> prev_ids;
        std::vector<std::string> next_ids;
        if (i == 0 && !cfg.context_previous_request_ids.empty())
            prev_ids = cfg.context_previous_request_ids;
        else
            prev_ids = prev_request_ids;
        if (prev_ids.empty()) {
            if (i == 0)
                prev_text = cfg.context_previous_text;
            else
                prev_text = text_chunks[i - 1];
        }
        if (i + 1 == text_chunks.size() && !cfg.context_next_request_ids.empty())
            next_ids = cfg.context_next_request_ids;
        else if (i + 1 < text_chunks.size())
            next_text = text_chunks[i + 1];
        else
            next_text = cfg.context_next_text;

        const std::string body_str = build_tts_body(
            cfg, text_chunks[i], prev_text, next_text, prev_ids, next_ids);

        const size_t bytes_before = response.size();
        std::string request_id;

        curl_easy_setopt(c, CURLOPT_URL,            url.c_str());
        curl_easy_setopt(c, CURLOPT_HTTPHEADER,     hdrs);
        curl_easy_setopt(c, CURLOPT_POSTFIELDS,     body_str.c_str());
        curl_easy_setopt(c, CURLOPT_POSTFIELDSIZE,  static_cast<long>(body_str.size()));
        curl_easy_setopt(c, CURLOPT_WRITEFUNCTION,  curl_write_tts_bytes);
        curl_easy_setopt(c, CURLOPT_WRITEDATA,      &trace);
        curl_easy_setopt(c, CURLOPT_HEADERFUNCTION, curl_capture_request_id);
        curl_easy_setopt(c, CURLOPT_HEADERDATA,     &request_id);
        curl_easy_setopt(c, CURLOPT_NOSIGNAL,       1L);
        curl_easy_setopt(c, CURLOPT_CONNECTTIMEOUT, C::k_curl_connect_timeout_s);
        curl_easy_setopt(c, CURLOPT_TIMEOUT,        C::k_curl_transfer_timeout_s);
        if (cfg.should_cancel) {
            curl_easy_setopt(c, CURLOPT_NOPROGRESS,       0L);
            curl_easy_setopt(c, CURLOPT_XFERINFOFUNCTION, curl_should_cancel_check);
            curl_easy_setopt(c, CURLOPT_XFERINFODATA,     &cfg.should_cancel);
        }

        const CURLcode rc = curl_easy_perform(c);

        long http_code = 0;
        curl_off_t ttfb_us = 0;
        curl_off_t total_us = 0;
        char* content_type = nullptr;
        curl_easy_getinfo(c, CURLINFO_RESPONSE_CODE, &http_code);
        curl_easy_getinfo(c, CURLINFO_STARTTRANSFER_TIME_T, &ttfb_us);
        curl_easy_getinfo(c, CURLINFO_TOTAL_TIME_T, &total_us);
        curl_easy_getinfo(c, CURLINFO_CONTENT_TYPE, &content_type);
        pm::audio::log::trace("[speak] elevenlabs buffered: chunk "
                      + std::to_string(i + 1) + "/" + std::to_string(text_chunks.size())
                      + " chars=" + std::to_string(text_chunks[i].size())
                      + " http=" + std::to_string(http_code)
                      + " model=" + cfg.model_id
                      + " format=" + cfg.output_format
                      + " content_type=" + std::string(content_type ? content_type : "(none)")
                      + " bytes=" + std::to_string(response.size() - bytes_before)
                      + " request_id=" + (request_id.empty() ? "(none)" : request_id)
                      + " ttfb=" + std::to_string(ttfb_us / 1000) + "ms"
                      + " total=" + std::to_string(total_us / 1000) + "ms");

        if (rc == CURLE_ABORTED_BY_CALLBACK || synthesis_cancelled(cfg)) {
            pm::audio::log::trace("[speak] elevenlabs buffered: cancelled during chunk "
                          + std::to_string(i + 1) + "/" + std::to_string(text_chunks.size()));
            curl_slist_free_all(hdrs);
            curl_easy_cleanup(c);
            throw_if_cancelled(cfg);
        }

        if (rc != CURLE_OK)
            fail(std::string("elevenlabs_tts: curl error: ") + curl_easy_strerror(rc));

        if (http_code != 200) {
            // Response body will be JSON error from the API.
            std::string msg(response.begin() + static_cast<std::ptrdiff_t>(bytes_before),
                            response.end());
            fail("elevenlabs_tts: HTTP " + std::to_string(http_code) + ": " + msg);
        }

        last_request_id = request_id;
        remember_request_id(prev_request_ids, std::move(request_id), cfg.max_prev_request_ids);
    }

    pm::audio::log::trace("[speak] elevenlabs buffered: complete"
                  " chunks=" + std::to_string(text_chunks.size())
                  + " bytes=" + std::to_string(response.size())
                  + " net_chunks=" + std::to_string(trace.chunks)
                  + " largest_net_chunk=" + std::to_string(trace.largest_chunk) + "B"
                  + " max_net_gap=" + std::to_string(trace.max_chunk_gap_ms) + "ms");
    curl_slist_free_all(hdrs);
    curl_easy_cleanup(c);

    TtsSynthResult out;
    out.audio = std::move(response);
    out.request_id = std::move(last_request_id);
    return out;
}

std::vector<uint8_t> elevenlabs_tts_synthesize(const std::string& text,
                                                const ElevenLabsTTSConfig& cfg)
{
    return elevenlabs_tts_synthesize_result(text, cfg).audio;
}

void elevenlabs_tts_synthesize_stream(const std::string&          text,
                                      const ElevenLabsTTSConfig&  cfg,
                                      pm::audio::AudioRingBuffer& ring)
{
    CURL* c = tls_curl_handle();
    if (!c) throw std::runtime_error("elevenlabs_tts_stream: curl_easy_init failed");
    curl_reset_per_request(c);

    // Use the /stream endpoint (not the bare /text-to-speech route). The bare
    // route waits for the full synthesis to finish before sending any bytes,
    // which defeats the ring buffer — pre-buffer fills late, then we play out
    // of a fully-buffered response with no further chunks arriving. The /stream
    // route emits bytes as they're generated, so the ring stays fed.
    const std::string url =
        "https://api.elevenlabs.io/v1/text-to-speech/" + cfg.voice_id + "/stream"
        + "?output_format=" + cfg.output_format;

    const std::string auth_hdr = "xi-api-key: " + cfg.api_key;
    const std::string ct_hdr   = "Content-Type: application/json";
    curl_slist* hdrs = nullptr;
    hdrs = curl_slist_append(hdrs, auth_hdr.c_str());
    hdrs = curl_slist_append(hdrs, ct_hdr.c_str());

    // With sentence chunking, chunk N+1 is requested only after chunk N has
    // fully streamed into the ring, so playback of chunk N overlaps the
    // synthesis of N+1 and perceived latency stays unchanged.
    const auto text_chunks = split_text_for_tts(text, cfg);
    std::vector<std::string> prev_request_ids;

    for (size_t i = 0; i < text_chunks.size(); ++i) {
        if (ring.cancelled.load(std::memory_order_acquire) || synthesis_cancelled(cfg)) {
            pm::audio::log::trace("[speak] elevenlabs: cancelled before chunk "
                          + std::to_string(i + 1) + "/" + std::to_string(text_chunks.size()));
            curl_slist_free_all(hdrs);
            return;
        }

        std::string prev_text;
        std::string next_text;
        std::vector<std::string> prev_ids;
        std::vector<std::string> next_ids;
        if (i == 0 && !cfg.context_previous_request_ids.empty())
            prev_ids = cfg.context_previous_request_ids;
        else
            prev_ids = prev_request_ids;
        if (prev_ids.empty()) {
            if (i == 0)
                prev_text = cfg.context_previous_text;
            else
                prev_text = text_chunks[i - 1];
        }
        if (i + 1 == text_chunks.size() && !cfg.context_next_request_ids.empty())
            next_ids = cfg.context_next_request_ids;
        else if (i + 1 < text_chunks.size())
            next_text = text_chunks[i + 1];
        else
            next_text = cfg.context_next_text;

        const std::string body_str = build_tts_body(
            cfg, text_chunks[i], prev_text, next_text, prev_ids, next_ids);
        std::string request_id;

        StreamRingWriteCtx write_ctx{&ring, {}, {}, true};
        StreamCancelCtx cancel_ctx{&ring, cfg.should_cancel};
        curl_easy_setopt(c, CURLOPT_URL,              url.c_str());
        curl_easy_setopt(c, CURLOPT_HTTPHEADER,       hdrs);
        curl_easy_setopt(c, CURLOPT_POSTFIELDS,       body_str.c_str());
        curl_easy_setopt(c, CURLOPT_POSTFIELDSIZE,    static_cast<long>(body_str.size()));
        curl_easy_setopt(c, CURLOPT_WRITEFUNCTION,    curl_write_ring_guarded);
        curl_easy_setopt(c, CURLOPT_WRITEDATA,        &write_ctx);
        curl_easy_setopt(c, CURLOPT_HEADERFUNCTION,   curl_header_stream);
        curl_easy_setopt(c, CURLOPT_HEADERDATA,       &write_ctx);
        curl_easy_setopt(c, CURLOPT_NOPROGRESS,       0L);
        curl_easy_setopt(c, CURLOPT_XFERINFOFUNCTION, curl_stream_cancel_check);
        curl_easy_setopt(c, CURLOPT_XFERINFODATA,     &cancel_ctx);
        curl_easy_setopt(c, CURLOPT_CONNECTTIMEOUT,   C::k_curl_connect_timeout_s);
        curl_easy_setopt(c, CURLOPT_TIMEOUT,          C::k_curl_transfer_timeout_s);

        // Timing: TTFB is the dominant TTS-side latency. With persistent handles
        // libcurl reuses the TCP+TLS session → typically saves 200–400 ms.
        const auto t_send = std::chrono::steady_clock::now();
        curl_off_t ttfb_us = 0;
        curl_off_t conn_us = 0;

        const CURLcode rc = curl_easy_perform(c);

        long http_code = 0;
        curl_easy_getinfo(c, CURLINFO_RESPONSE_CODE, &http_code);
        curl_easy_getinfo(c, CURLINFO_STARTTRANSFER_TIME_T, &ttfb_us);
        curl_easy_getinfo(c, CURLINFO_CONNECT_TIME_T,       &conn_us);
        const auto total_ms = std::chrono::duration_cast<std::chrono::milliseconds>(
            std::chrono::steady_clock::now() - t_send).count();

        if (ring.cancelled.load(std::memory_order_acquire)
            || synthesis_cancelled(cfg)
            || rc == CURLE_ABORTED_BY_CALLBACK) {
            pm::audio::log::trace("[speak] elevenlabs: cancelled (total="
                          + std::to_string(total_ms) + "ms)");
            curl_slist_free_all(hdrs);
            return;
        }

        request_id = std::move(write_ctx.request_id);
        pm::audio::log::trace("[speak] elevenlabs: chunk "
                      + std::to_string(i + 1) + "/" + std::to_string(text_chunks.size())
                      + " chars=" + std::to_string(text_chunks[i].size())
                      + " http=" + std::to_string(http_code)
                      + " request_id=" + (request_id.empty() ? "(none)" : request_id)
                      + " connect=" + std::to_string(conn_us / 1000) + "ms"
                      + " ttfb=" + std::to_string(ttfb_us / 1000) + "ms"
                      + " total=" + std::to_string(total_ms) + "ms"
                      + " (conn=0 means reused)");

        if (rc != CURLE_OK) {
            curl_slist_free_all(hdrs);
            throw std::runtime_error(std::string("elevenlabs_tts_stream: curl error: ")
                                     + curl_easy_strerror(rc));
        }
        if (http_code != 200) {
            curl_slist_free_all(hdrs);
            std::string err_body;
            if (!write_ctx.error_buf.empty()) {
                const size_t clip = std::min(write_ctx.error_buf.size(), static_cast<size_t>(512));
                err_body.assign(reinterpret_cast<const char*>(write_ctx.error_buf.data()), clip);
            }
            throw std::runtime_error("elevenlabs_tts_stream: HTTP "
                                     + std::to_string(http_code)
                                     + (err_body.empty() ? "" : ": " + err_body));
        }

        remember_request_id(prev_request_ids, std::move(request_id), cfg.max_prev_request_ids);
    }

    curl_slist_free_all(hdrs);
}

} // namespace pm::tts

