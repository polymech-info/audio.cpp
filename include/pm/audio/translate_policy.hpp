#pragma once
//
// Policy shared by the two translator backends:
//   miniaudio — RealtimeTranslateSession (packages/audio.cpp/src/translate.cpp)
//   AAudio    — pm::android live session (src/android/pm_android_translate.cpp)
//
// Device open, resample, and the playback queue stay in each backend.
// Silence detection and the half-duplex thresholds do not.

#include <cstddef>
#include <cstdint>
#include <cmath>

namespace pm::audio {

struct ModelPcmLevel {
    int peak = 0;
    double rms = 0;
    bool silence = true;
};

// Provider digital silence. Queuing it keeps the speaker path — and the mic
// mute — from ever draining.
inline ModelPcmLevel model_pcm_level(const std::int16_t* pcm, std::size_t frames) {
    ModelPcmLevel out;
    if (!pcm || frames == 0)
        return out;
    std::uint64_t sum_sq = 0;
    int peak = 0;
    for (std::size_t i = 0; i < frames; ++i) {
        const int sample = static_cast<int>(pcm[i]);
        const int magnitude = sample < 0 ? -sample : sample;
        if (magnitude > peak)
            peak = magnitude;
        sum_sq += static_cast<std::uint64_t>(
            static_cast<std::int64_t>(sample) * static_cast<std::int64_t>(sample));
    }
    out.peak = peak;
    out.rms = std::sqrt(static_cast<double>(sum_sq) / static_cast<double>(frames));
    constexpr int kPeak = 32;
    constexpr double kRms = 4.0;
    out.silence = peak <= kPeak && out.rms <= kRms;
    return out;
}

} // namespace pm::audio
