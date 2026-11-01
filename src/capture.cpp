#include "pm/audio/capture.hpp"

#include "pm/audio/constants.hpp"

#include <algorithm>
#include <cctype>

namespace pm::audio {
namespace {

std::string lower_ascii(std::string s)
{
    for (char& c : s)
        c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    return s;
}

} // namespace

CaptureSource parse_capture_source(const std::string& token, CaptureSource fallback)
{
    const std::string s = lower_ascii(token);
    if (s.empty() || s == "none" || s == "off")
        return CaptureSource::none;
    if (s == "mic" || s == "microphone")
        return CaptureSource::mic;
    if (s == "desktop" || s == "system" || s == "loopback")
        return CaptureSource::desktop;
    if (s == "mix" || s == "both")
        return CaptureSource::mix;
    return fallback;
}

const char* capture_source_name(CaptureSource src) noexcept
{
    switch (src) {
    case CaptureSource::none: return "none";
    case CaptureSource::mic: return "mic";
    case CaptureSource::desktop: return "desktop";
    case CaptureSource::mix: return "mix";
    }
    return "none";
}

bool capture_source_active(const MixedCaptureConfig& cfg) noexcept
{
    return cfg.source != CaptureSource::none || cfg.camera_audio;
}

bool capture_has_speech_mic(const MixedCaptureConfig& cfg) noexcept
{
    if (cfg.camera_audio)
        return true;
    return cfg.source == CaptureSource::mic || cfg.source == CaptureSource::mix;
}

void resolve_capture_pcm_format(const MixedCaptureConfig& cfg,
                                uint32_t& sample_rate,
                                uint32_t& channels) noexcept
{
    if (cfg.sample_rate >= 8000 && cfg.channels >= 1) {
        sample_rate = cfg.sample_rate;
        channels = std::min(cfg.channels, 2u);
        return;
    }
    if (cfg.source == CaptureSource::desktop ||
        (cfg.source == CaptureSource::none && cfg.camera_audio)) {
        sample_rate = constants::k_recording_sample_rate;
        channels = constants::k_recording_channels;
        return;
    }
    sample_rate = constants::k_capture_sample_rate;
    channels = constants::k_capture_channels;
}

std::string capture_pcm_format_string(uint32_t sample_rate, uint32_t channels)
{
    const char* ch_name = (channels == 1) ? "mono" : "stereo";
    return "s16le " + std::string(ch_name) + " " + std::to_string(sample_rate) + " Hz";
}

} // namespace pm::audio
