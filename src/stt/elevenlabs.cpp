#include "pm/audio/stt/elevenlabs.hpp"

#include <curl/curl.h>
#include <curl/websockets.h>
#include <nlohmann/json.hpp>

#include <atomic>
#include <chrono>
#include <cmath>
#include <condition_variable>
#include <cstdint>
#include <mutex>
#include <queue>
#include <stdexcept>
#include <string>
#include <thread>

namespace pm::stt {

// ── base64 ────────────────────────────────────────────────────────────────────

static const char k_b64[] =
    "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";

static std::string b64_encode(const void* data, size_t len) {
    const auto* p = static_cast<const uint8_t*>(data);
    std::string out;
    out.reserve(((len + 2) / 3) * 4);
    for (size_t i = 0; i < len; i += 3) {
        uint32_t n = static_cast<uint32_t>(p[i]) << 16;
        if (i + 1 < len) n |= static_cast<uint32_t>(p[i + 1]) << 8;
        if (i + 2 < len) n |= static_cast<uint32_t>(p[i + 2]);
        out += k_b64[(n >> 18) & 0x3f];
        out += k_b64[(n >> 12) & 0x3f];
        out += (i + 1 < len) ? k_b64[(n >> 6) & 0x3f] : '=';
        out += (i + 2 < len) ? k_b64[(n >> 0) & 0x3f] : '=';
    }
    return out;
}

// ── Impl ──────────────────────────────────────────────────────────────────────

struct ElevenLabsSTT::Impl {
    CURL*  curl = nullptr;
    Config cfg;

    std::atomic<bool> running{false};
    std::atomic<bool> connected{false};
    std::thread       thread;

    // session_started synchronisation
    std::mutex              session_mtx;
    std::condition_variable session_cv;
    std::atomic<bool>       session_ok{false};

    // outgoing JSON text frames — queued from any thread, drained by event loop
    std::mutex              send_mtx;
    std::queue<std::string> send_queue;

    // partial frame accumulation (one WebSocket message may arrive in pieces)
    std::string frame_buf;

    // callbacks (snapshot at connect time so the event thread holds them safely)
    std::function<void()>                   cb_session_started;
    std::function<void(const std::string&)> cb_partial;
    std::function<void(const std::string&)> cb_committed;
    std::function<void(const ElevenLabsSTT::TimedCommit&)> cb_committed_timed;
    std::function<void(const std::string&)> cb_error;
    std::function<void()>                   cb_close;

    static std::int64_t json_sec_to_ms(const nlohmann::json& v)
    {
        if (v.is_number_float())
            return static_cast<std::int64_t>(std::llround(v.get<double>() * 1000.0));
        if (v.is_number_integer())
            return v.get<std::int64_t>() * 1000;
        if (v.is_string()) {
            try {
                return static_cast<std::int64_t>(
                    std::llround(std::stod(v.get<std::string>()) * 1000.0));
            } catch (...) {
                return 0;
            }
        }
        return 0;
    }

    static ElevenLabsSTT::TimedCommit parse_timed_commit(const nlohmann::json& j)
    {
        ElevenLabsSTT::TimedCommit out;
        out.text = j.value("text", std::string{});
        const nlohmann::json* arr = nullptr;
        if (j.contains("words") && j["words"].is_array())
            arr = &j["words"];
        else if (j.contains("tokens") && j["tokens"].is_array())
            arr = &j["tokens"];
        if (!arr)
            return out;
        for (const auto& e : *arr) {
            if (!e.is_object())
                continue;
            const std::string typ = e.value("type", std::string{"word"});
            if (typ == "audio_event")
                continue;
            ElevenLabsSTT::TimedWord w;
            w.text = e.value("text", e.value("word", std::string{}));
            if (e.contains("start_ms"))
                w.t0_ms = e.value("start_ms", 0);
            else if (e.contains("start"))
                w.t0_ms = json_sec_to_ms(e["start"]);
            if (e.contains("end_ms"))
                w.t1_ms = e.value("end_ms", 0);
            else if (e.contains("end"))
                w.t1_ms = json_sec_to_ms(e["end"]);
            if (w.t1_ms < w.t0_ms)
                w.t1_ms = w.t0_ms;
            if (w.text.empty() && typ == "spacing")
                continue;
            out.words.push_back(std::move(w));
        }
        return out;
    }

    // ── helpers ──────────────────────────────────────────────────────────────

    void enqueue(std::string msg) {
        std::lock_guard<std::mutex> lk(send_mtx);
        send_queue.push(std::move(msg));
    }

    // Drain every queued frame through curl_ws_send (called from event thread).
    void flush_send_queue() {
        std::unique_lock<std::mutex> lk(send_mtx);
        while (!send_queue.empty()) {
            auto msg = std::move(send_queue.front());
            send_queue.pop();
            lk.unlock();
            size_t sent = 0;
            curl_ws_send(curl, msg.data(), msg.size(), &sent, 0, CURLWS_TEXT);
            lk.lock();
        }
    }

    void dispatch(const std::string& raw) {
        try {
            auto j   = nlohmann::json::parse(raw);
            auto typ = j.value("message_type", std::string{});

            if (typ == "session_started") {
                {
                    std::lock_guard<std::mutex> lk(session_mtx);
                    session_ok.store(true);
                }
                session_cv.notify_all();
                if (cb_session_started) cb_session_started();

            } else if (typ == "partial_transcript") {
                if (cb_partial) cb_partial(j.value("text", std::string{}));

            } else if (typ == "committed_transcript") {
                if (cb_committed) cb_committed(j.value("text", std::string{}));

            } else if (typ == "committed_transcript_with_timestamps") {
                auto timed = parse_timed_commit(j);
                if (cb_committed_timed)
                    cb_committed_timed(timed);
                if (cb_committed) {
                    const std::string& text = timed.text.empty()
                        ? j.value("text", std::string{})
                        : timed.text;
                    if (!text.empty())
                        cb_committed(text);
                }

            } else if (typ == "input_error") {
                if (cb_error) cb_error(j.dump());
            }
        } catch (...) {
            // non-JSON frame (ping, control) — ignore
        }
    }

    // ── event loop (background thread) ───────────────────────────────────────

    void recv_loop() {
        char tmp[16384];

        while (running.load()) {
            flush_send_queue();

            size_t nrecvd = 0;
            const curl_ws_frame* meta = nullptr;
            CURLcode rc = curl_ws_recv(curl, tmp, sizeof(tmp), &nrecvd, &meta);

            if (rc == CURLE_AGAIN) {
                std::this_thread::sleep_for(std::chrono::milliseconds(2));
                continue;
            }
            if (rc != CURLE_OK) {
                if (cb_error)
                    cb_error(std::string("curl_ws_recv: ") + curl_easy_strerror(rc));
                break;
            }
            if (meta && (meta->flags & CURLWS_CLOSE)) {
                break;
            }

            frame_buf.append(tmp, nrecvd);

            // bytesleft == 0 → complete message (may have arrived in chunks)
            if (meta && meta->bytesleft == 0 && !frame_buf.empty()) {
                dispatch(frame_buf);
                frame_buf.clear();
            }
        }

        running.store(false);
        connected.store(false);
        if (cb_close) cb_close();
    }
};

// ── ElevenLabsSTT ─────────────────────────────────────────────────────────────

ElevenLabsSTT::ElevenLabsSTT()  : impl_(std::make_unique<Impl>()) {}
ElevenLabsSTT::~ElevenLabsSTT() { close(); }

void ElevenLabsSTT::connect(const Config& cfg) {
    // Snapshot callbacks so the event thread has a stable reference.
    impl_->cfg               = cfg;
    impl_->cb_session_started = on_session_started;
    impl_->cb_partial         = on_partial;
    impl_->cb_committed       = on_committed;
    impl_->cb_committed_timed = on_committed_timed;
    impl_->cb_error           = on_error;
    impl_->cb_close           = on_close;

    std::string url =
        "wss://api.elevenlabs.io/v1/speech-to-text/realtime?model_id=" + cfg.model_id;
    if (cfg.include_timestamps)
        url += "&include_timestamps=true";
    const std::string key_hdr = "xi-api-key: " + cfg.api_key;

    CURL* c = curl_easy_init();
    if (!c)
        throw std::runtime_error("ElevenLabs STT: curl_easy_init failed");
    impl_->curl = c;

    curl_slist* hdrs = nullptr;
    hdrs = curl_slist_append(hdrs, key_hdr.c_str());

    curl_easy_setopt(c, CURLOPT_URL,            url.c_str());
    curl_easy_setopt(c, CURLOPT_HTTPHEADER,     hdrs);
    curl_easy_setopt(c, CURLOPT_CONNECT_ONLY,   2L);   // 2 = WebSocket upgrade
    curl_easy_setopt(c, CURLOPT_NOSIGNAL,       1L);
    curl_easy_setopt(c, CURLOPT_TIMEOUT,        0L);   // no overall timeout
    curl_easy_setopt(c, CURLOPT_CONNECTTIMEOUT, 10L);  // TCP/TLS connect ≤ 10 s

    // TLS handshake + WebSocket upgrade
    CURLcode rc = curl_easy_perform(c);
    curl_slist_free_all(hdrs);

    if (rc != CURLE_OK) {
        curl_easy_cleanup(c);
        impl_->curl = nullptr;
        throw std::runtime_error(
            std::string("ElevenLabs STT: WebSocket connect failed: ") +
            curl_easy_strerror(rc));
    }

    // Start the background event loop.
    impl_->running.store(true);
    impl_->thread = std::thread([this] { impl_->recv_loop(); });

    // Block until session_started arrives (or 10 s timeout).
    // The predicate also checks !running so that close() can interrupt the wait.
    {
        std::unique_lock<std::mutex> lk(impl_->session_mtx);
        const bool ok = impl_->session_cv.wait_for(lk, std::chrono::seconds(10),
            [this] { return impl_->session_ok.load() || !impl_->running.load(); });
        if (!impl_->session_ok.load()) {
            // Either timed-out or close() was called (cancelled).
            close();
            if (ok) return; // cancelled by close() — not an error for the caller
            throw std::runtime_error(
                "ElevenLabs STT: timeout waiting for session_started (10 s)");
        }
    }

    impl_->connected.store(true);
}

void ElevenLabsSTT::send_pcm(const int16_t* pcm, size_t frames) {
    if (!impl_->running.load()) return;

    nlohmann::json msg;
    msg["message_type"] = "input_audio_chunk";
    msg["audio_base_64"] = b64_encode(pcm, frames * sizeof(int16_t));
    msg["commit"]        = false;
    msg["sample_rate"]   = static_cast<int>(impl_->cfg.sample_rate);
    impl_->enqueue(msg.dump());
}

void ElevenLabsSTT::commit() {
    if (!impl_->running.load()) return;

    nlohmann::json msg;
    msg["message_type"] = "input_audio_chunk";
    msg["audio_base_64"] = "";
    msg["commit"]        = true;
    msg["sample_rate"]   = static_cast<int>(impl_->cfg.sample_rate);
    impl_->enqueue(msg.dump());
}

void ElevenLabsSTT::close() {
    impl_->running.store(false);
    impl_->session_cv.notify_all();  // unblock any waiting connect()

    if (impl_->thread.joinable())
        impl_->thread.join();

    if (impl_->curl) {
        size_t sent = 0;
        curl_ws_send(impl_->curl, "", 0, &sent, 0, CURLWS_CLOSE);
        curl_easy_cleanup(impl_->curl);
        impl_->curl = nullptr;
    }
    impl_->connected.store(false);
}

bool ElevenLabsSTT::is_connected() const {
    return impl_->connected.load();
}

} // namespace pm::stt

