#pragma once

#include <cstdint>
#include <string>

namespace pm::audio {

enum class CaptureSource {
    none,
    mic,
    desktop,
    mix,
};

struct MixedCaptureConfig {
    CaptureSource source = CaptureSource::mic;
    std::string mic_device;
    std::string desktop_device;
    float mic_gain = 1.0f;
    float desktop_gain = 1.0f;
    bool camera_audio = false;
    std::string camera_device;
    float camera_gain = 1.0f;
    uint32_t sample_rate = 0;
    uint32_t channels = 0;
};

CaptureSource parse_capture_source(const std::string& token,
                                   CaptureSource fallback = CaptureSource::mic);
const char* capture_source_name(CaptureSource src) noexcept;
bool capture_source_active(const MixedCaptureConfig& cfg) noexcept;
bool capture_has_speech_mic(const MixedCaptureConfig& cfg) noexcept;
void resolve_capture_pcm_format(const MixedCaptureConfig& cfg,
                                uint32_t& sample_rate,
                                uint32_t& channels) noexcept;
std::string capture_pcm_format_string(uint32_t sample_rate, uint32_t channels);

} // namespace pm::audio
