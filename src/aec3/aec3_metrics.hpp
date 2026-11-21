#pragma once

#include <cstddef>
#include <cstdint>
#include <vector>

namespace pm::audio::aec3 {

struct Level {
    double rms = 0;
    double peak = 0;
    std::uint64_t clips = 0;
    std::uint64_t samples = 0;
};

struct LagEstimate {
    int lag_samples = 0;
    double correlation = 0;
    bool valid = false;
    bool reliable = false;
};

Level measure_level(const std::int16_t* pcm, std::size_t count);
LagEstimate estimate_lag(const std::int16_t* render, std::size_t render_count,
                         const std::int16_t* capture, std::size_t capture_count,
                         int max_lag_samples, int step);

} // namespace pm::audio::aec3
