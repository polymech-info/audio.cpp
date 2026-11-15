#pragma once
//
// Speech activity + translate playback gate.
//
// WebRtcVad_* when HAVE_WEBRTC_VAD; otherwise an RMS fallback with the same
// state machine. Hosts only see speech-start / speech-end / activity events —
// no WebRTC types leak.

#include <cstddef>
#include <cstdint>
#include <functional>
#include <memory>
#include <string>
#include <vector>

namespace pm::audio {

enum class PlaybackMode {
    simultaneous,   // play translated PCM as it arrives
    conversational, // hold until end-of-speech silence, then release
};

struct VadFrameConfig {
    int sample_rate_hz = 16000;     // 8000 / 16000 / 32000 / 48000
    int frame_ms = 20;              // 10 / 20 / 30
    int mode = 1;                   // WebRTC aggressiveness 0..3
    int speech_onset_ms = 60;
    int preroll_ms = 200;
    int silence_ms = 500;           // end-of-speech (200..2000)
    int min_utterance_ms = 200;
    int max_segment_ms = 10000;
    float rms_fallback_threshold = 200.f; // used when WebRTC VAD unavailable
};

enum class SpeechEvent {
    speech_start,
    speech_end,
    activity, // optional level tick
};

struct SpeechActivity {
    using Listener = std::function<void(SpeechEvent, float level)>;

    SpeechActivity();
    ~SpeechActivity();
    SpeechActivity(const SpeechActivity&) = delete;
    SpeechActivity& operator=(const SpeechActivity&) = delete;

    bool open(const VadFrameConfig& cfg, std::string& err);
    void close();
    bool ok() const;

    // Feed mono PCM at cfg.sample_rate_hz. Returns true when the latest frame
    // was classified as speech (after onset hysteresis).
    bool feed(const std::int16_t* pcm, std::size_t frames);
    bool in_speech() const;
    // Drop in-progress onset / utterance without emitting speech_end.
    void reset_turn();
    const char* backend() const; // "webrtc" | "rms"

    void set_listener(Listener cb);

private:
    struct Impl;
    std::unique_ptr<Impl> m_;
};

// Option A: buffer translated PCM until SpeechActivity reports speech_end,
// then release into the playback sink and keep streaming.
class TranslatePlaybackGate {
public:
    struct Config {
        PlaybackMode mode = PlaybackMode::simultaneous;
        VadFrameConfig vad;
        std::size_t max_buffered_frames = 24000 * 60; // ~60 s @ 24 kHz
        int translated_rate_hz = 24000;               // for VAD suppress while playing
    };

    using Sink = std::function<void(const std::int16_t* pcm, std::size_t frames)>;
    using VadHook = std::function<void(SpeechEvent, float level, std::size_t held_frames)>;
    // Host-owned playback truth: queued PCM or recently rendered PCM. This is
    // more reliable than estimating playout from network chunk sizes.
    using PlaybackActive = std::function<bool()>;

    TranslatePlaybackGate();
    ~TranslatePlaybackGate();
    TranslatePlaybackGate(const TranslatePlaybackGate&) = delete;
    TranslatePlaybackGate& operator=(const TranslatePlaybackGate&) = delete;

    bool open(const Config& cfg, Sink sink, std::string& err);
    void close();
    void set_vad_hook(VadHook hook);
    void set_playback_active(PlaybackActive cb);

    // Mic leg (AEC-cleaned preferred). Drives VAD in conversational mode.
    // Skipped while we are releasing held audio (stops speaker→mic false speech).
    void feed_mic(const std::int16_t* pcm, std::size_t frames, int rate_hz);

    // Translated PCM from the model (any rate; stored and released as-is).
    void feed_translated(const std::int16_t* pcm, std::size_t frames);

    // Flush any held audio (session stop / close).
    void flush();

    PlaybackMode mode() const;
    bool holding() const;
    // True while the host reports queued or recently rendered translation.
    // Hosts keep protocol cadence with zero PCM instead of forwarding the mic.
    bool uplink_muted() const;
    std::size_t buffered_frames() const;
    const char* vad_backend() const;

private:
    struct Impl;
    std::unique_ptr<Impl> m_;
};

inline PlaybackMode parse_playback_mode(const std::string& s) {
    if (s == "conversational" || s == "after_silence" || s == "vad")
        return PlaybackMode::conversational;
    return PlaybackMode::simultaneous;
}

inline const char* playback_mode_name(PlaybackMode m) {
    return m == PlaybackMode::conversational ? "conversational" : "simultaneous";
}

} // namespace pm::audio
