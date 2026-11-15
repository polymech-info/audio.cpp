#pragma once
//
// RealtimeTranslateSession — mic + speaker + kbot TranslationSession.
//
// Live speech-to-speech translation (interpreter), not a voice agent.
// Protocol lives in packages/llm.cpp (TranslationSession); this file owns devices.
//
//     mic (AudioInput, 16k) ──resample 24k──▶ TranslationSession::send_pcm
//     TranslationSession::on_audio_pcm (24k) ──▶ speaker (PCM ring → ma_device)
//
// Host-compiled under FEATURE_REALTIME (needs kbot). Not in the default
// pm-audio-lib glob — same pattern as mixed_capture / RealtimeVoiceSession.

#if defined(FEATURE_REALTIME) && FEATURE_REALTIME

#include <cstdint>
#include <functional>
#include <memory>
#include <string>

namespace pm::audio {

class RealtimeTranslateSession {
public:
    struct Config {
        std::string base_url;
        std::string api_key;
        std::string model;      // tanit-realtime-translate / gpt-realtime-translate
        std::string language;   // target language, e.g. "es"
        std::string noise_reduction = "near_field";
        std::string transcription_model;
        std::string mic_device;     // substring; empty = default
        std::string speaker_device; // substring; empty = default (single device)
        float       mic_gain = 1.0f;
        int         close_drain_timeout_ms = 30000;
    };

    struct Callbacks {
        std::function<void()>                   on_ready;
        std::function<void(const std::string&)> on_output_transcript; // translated
        std::function<void(const std::string&)> on_input_transcript;  // source
        std::function<void(double)>             on_level;             // mic [0,1]
        std::function<void(const std::string&)> on_error;
        std::function<void(const std::string&)> on_auth_error;
        std::function<void(const std::string&)> on_network_error;
        std::function<void()>                   on_closed;
    };

    RealtimeTranslateSession();
    ~RealtimeTranslateSession();
    RealtimeTranslateSession(const RealtimeTranslateSession&) = delete;
    RealtimeTranslateSession& operator=(const RealtimeTranslateSession&) = delete;

    bool start(const Config& cfg, Callbacks cb, std::string& err);
    // Prefer graceful drain (session.close → session.closed) then teardown.
    void stop();
    void update_language(const std::string& language);
    bool is_active() const;

private:
    struct Impl;
    std::unique_ptr<Impl> m_;
};

} // namespace pm::audio

#endif // FEATURE_REALTIME
