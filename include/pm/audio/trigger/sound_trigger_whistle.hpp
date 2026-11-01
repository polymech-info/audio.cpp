#pragma once

// Double-whistle detector. The engine and inspect path call this; the
// ridge tracker itself stays inside sound_trigger_whistle.cpp.

#include "pm/audio/trigger/sound_trigger.hpp"

#include <atomic>
#include <cstdint>
#include <memory>
#include <sstream>
#include <string>
#include <vector>

namespace pm::audio::trigger {

inline std::string next_operation_id()
{
    static std::atomic<std::uint64_t> serial{0};
    const auto n = serial.fetch_add(1, std::memory_order_relaxed) + 1;
    std::ostringstream out;
    out << "sound-" << std::hex << n;
    return out.str();
}

void add_spectral_metrics(const SpectrumBins& power, std::uint32_t sample_rate,
                          float min_hz, float max_hz, FrameMetrics& metrics);

// One tracker per loaded double-whistle profile. Pairing state lives here
// so the engine does not know about ridges.
class WhistleTracker {
public:
    WhistleTracker();
    ~WhistleTracker();
    WhistleTracker(WhistleTracker&&) noexcept;
    WhistleTracker& operator=(WhistleTracker&&) noexcept;
    WhistleTracker(const WhistleTracker&) = delete;
    WhistleTracker& operator=(const WhistleTracker&) = delete;

    void observe(const FrameMetrics& base,
                 const SpectrumBins* spectrum,
                 std::uint32_t sample_rate,
                 std::uint64_t frame_ms,
                 const WhistleConfig& cfg,
                 const std::string& trigger_id,
                 bool flush,
                 std::vector<Detection>& detections);

    // Writes ridge fields onto one inspect frame. flush closes an open tone.
    void inspect(const SpectrumBins* spectrum,
                 FrameMetrics& frame,
                 std::uint32_t sample_rate,
                 std::uint64_t frame_ms,
                 const WhistleConfig& cfg,
                 bool flush);

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

} // namespace pm::audio::trigger
