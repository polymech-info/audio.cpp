#pragma once

// A gap is bytes missing for the format the session opened.
// The check uses the sample rate, the channel count, the bit depth, and the
// byte count. It does not read sample values, and it does not need a side channel.

#include <cstdint>

namespace pm::audio {

struct StreamWindow {
    std::uint32_t sample_rate = 48000;
    std::uint32_t channels = 2;
    std::uint32_t bits = 16;
    std::uint64_t elapsed_ms = 0;
    std::uint64_t received_bytes = 0;
};

struct StreamGap {
    std::uint64_t expected_bytes = 0;
    std::uint64_t shortfall = 0;
    bool underrun = false;
};

/// `hold_percent` is how much of the expected audio must arrive.
/// 70 means a window under 70 percent of the format rate is an underrun.
inline StreamGap measure_stream_gap(const StreamWindow& window, std::uint32_t hold_percent = 70)
{
    StreamGap gap;
    if (window.sample_rate == 0 || window.channels == 0 || window.bits < 8 || (window.bits % 8) != 0)
        return gap;
    if (hold_percent > 100)
        hold_percent = 100;
    const std::uint64_t per_second = static_cast<std::uint64_t>(window.sample_rate) * window.channels
                                     * (window.bits / 8);
    gap.expected_bytes = per_second * window.elapsed_ms / 1000;
    if (window.received_bytes < gap.expected_bytes)
        gap.shortfall = gap.expected_bytes - window.received_bytes;
    gap.underrun = window.received_bytes * 100 < gap.expected_bytes * hold_percent;
    return gap;
}

} // namespace pm::audio
