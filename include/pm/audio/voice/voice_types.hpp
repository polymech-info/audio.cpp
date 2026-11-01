#pragma once

#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

namespace pm::audio::voice {

enum class ControlIntent {
    none,
    cancel,
    stop_listening,
};

enum class PipelineState {
    stopped,
    starting,
    listening,
    processing,
    stopping,
    failed,
};

struct PcmFormat {
    uint32_t sample_rate = 48000;
    uint16_t channels = 1;
};

struct PcmBuffer {
    PcmFormat format;
    std::vector<int16_t> samples;

    size_t frames() const noexcept
    {
        return format.channels == 0 ? 0 : samples.size() / format.channels;
    }
};

struct VadConfig {
    float rms_threshold = 200.0f;
    uint32_t speech_start_ms = 80;
    uint32_t silence_ms = 450;
    uint32_t preroll_ms = 250;
    uint32_t min_utterance_ms = 180;
    uint32_t max_utterance_ms = 15000;
};

struct MatchConfig {
    std::vector<std::string> wake_prefixes;
    std::vector<std::string> cancel_phrases{"stop", "cancel", "never mind", "nevermind"};
    std::vector<std::string> stop_phrases{"stop listening", "go to sleep"};
    bool wake_required = true;
};

struct PipelineConfig {
    PcmFormat capture_format;
    VadConfig vad;
    MatchConfig match;
    std::string route = "trigger";
    std::string requested_filter = "off";
    std::string effective_filter = "off";
    size_t callback_queue_ms = 2000;
    size_t worker_chunk_ms = 20;
    bool enhance_completed_utterances = true;
    bool enhancement_required = false;
    bool flush_utterance_on_stop = false;
    /// Wet/dry mix after utterance enhancement. 0 = original PCM, 1 = full
    /// filter (the previous implicit GTCRN/DeepFilter behaviour).
    float filter_blend = 1.0f;
};

inline float clamp_filter_blend(float value) noexcept
{
    if (value != value)
        return 1.0f;
    if (value < 0.0f)
        return 0.0f;
    if (value > 1.0f)
        return 1.0f;
    return value;
}

struct Transcript {
    std::string text;
    float confidence = 0.0f;
    std::string language;
};

struct MatchResult {
    bool matched = false;
    std::string normalized_text;
    std::string command_text;
    std::string wake_prefix;
    ControlIntent control = ControlIntent::none;
};

struct TriggerEvent {
    std::string id;
    uint64_t timestamp_ms = 0;
    std::string route;
    std::string text;
    std::string raw_text;
    std::string wake_prefix;
    float confidence = 0.0f;
    std::string language;
    std::string source = "voice";
    std::string requested_filter;
    std::string effective_filter;
    std::string filter_fallback;
};

struct CancelEvent {
    std::string id;
    uint64_t timestamp_ms = 0;
    std::string reason;
    std::string source = "voice";
};

struct PipelineStats {
    uint64_t callback_frames = 0;
    uint64_t dropped_frames = 0;
    uint64_t completed_utterances = 0;
    uint64_t rejected_utterances = 0;
};

const char* control_intent_name(ControlIntent value) noexcept;
const char* pipeline_state_name(PipelineState value) noexcept;

} // namespace pm::audio::voice
