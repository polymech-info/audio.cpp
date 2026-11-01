#pragma once

#include <cstdint>
#include <functional>
#include <memory>
#include <string>
#include <vector>

// ElevenLabs Scribe v2 Realtime STT over WebSocket (libcurl WS backend).
//
// Protocol:  wss://api.elevenlabs.io/v1/speech-to-text/realtime?model_id=…
// Auth:      xi-api-key header
// Send:      { "message_type": "input_audio_chunk", "audio_base_64": "<b64>",
//              "commit": false, "sample_rate": 16000 }
// Commit:    same with "audio_base_64":"", "commit":true
// Receive:   session_started | partial_transcript | committed_transcript |
//            committed_transcript_with_timestamps | input_error
//
// Thread model: connect() starts a background event loop thread that handles
// all curl I/O.  send_pcm() and commit() are safe to call from any thread
// (they enqueue JSON frames via a mutex-protected queue drained by the loop).

namespace pm::stt {

class ElevenLabsSTT {
public:
    struct Config {
        std::string api_key;
        std::string model_id    = "scribe_v2_realtime";
        uint32_t    sample_rate = 16000;
        /// Query include_timestamps=true → delayed committed_transcript_with_timestamps.
        bool        include_timestamps = false;
    };

    struct TimedWord {
        std::string text;
        std::int64_t t0_ms = 0;
        std::int64_t t1_ms = 0;
    };

    struct TimedCommit {
        std::string text;
        std::vector<TimedWord> words;
    };

    ElevenLabsSTT();
    ~ElevenLabsSTT();

    ElevenLabsSTT(const ElevenLabsSTT&)            = delete;
    ElevenLabsSTT& operator=(const ElevenLabsSTT&) = delete;

    // ── callbacks — set before connect() ───────────────────────────────────
    std::function<void()>                   on_session_started;
    /// Live partial transcript (may be empty string between words).
    std::function<void(const std::string&)> on_partial;
    /// Finalized utterance text.
    std::function<void(const std::string&)> on_committed;
    /// Word times from committed_transcript_with_timestamps (needs include_timestamps).
    std::function<void(const TimedCommit&)> on_committed_timed;
    std::function<void(const std::string&)> on_error;
    std::function<void()>                   on_close;

    // ── lifecycle ───────────────────────────────────────────────────────────

    // Connect TLS WebSocket, start event thread, wait for session_started
    // (max 10 s).  Throws std::runtime_error on failure.
    void connect(const Config& cfg);

    // Encode PCM as base64 and queue an input_audio_chunk frame.  Thread-safe.
    void send_pcm(const int16_t* pcm, size_t frames);

    // Queue a final commit frame (empty audio, commit=true) to flush the
    // utterance and receive the committed_transcript.
    void commit();

    // Stop the event loop, close the WebSocket, release curl handle.
    void close();

    bool is_connected() const;

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

} // namespace pm::stt
