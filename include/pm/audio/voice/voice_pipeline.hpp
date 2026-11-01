#pragma once

#include "pm/audio/voice/voice_adapters.hpp"

#include <chrono>
#include <memory>
#include <string>

namespace pm::audio::voice {

class VoicePipeline {
public:
    VoicePipeline(PipelineConfig config, VoiceAdapters adapters);
    ~VoicePipeline();

    VoicePipeline(const VoicePipeline&) = delete;
    VoicePipeline& operator=(const VoicePipeline&) = delete;

    bool start(const pm::audio::MixedCaptureConfig& capture_config, std::string& error);
    bool start_replay(std::string& error);

    void stop();
    bool is_running() const noexcept;
    PipelineState state() const noexcept;

    bool inject_pcm(const int16_t* interleaved_pcm, size_t frames) noexcept;

    bool replay_pcm(const PcmBuffer& pcm,
                    std::chrono::milliseconds timeout,
                    std::string& error);

    void finish_injected_utterance() noexcept;
    void inject_text(const Transcript& transcript);
    void cancel_current(std::string reason = "cancelled");

    bool wait_idle(std::chrono::milliseconds timeout);
    PipelineStats stats() const noexcept;

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

} // namespace pm::audio::voice
