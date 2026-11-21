#include "aec3_metrics.hpp"

#include <algorithm>
#include <cmath>
#include <cstdlib>

namespace pm::audio::aec3 {

Level measure_level(const std::int16_t* pcm, std::size_t count)
{
    Level level;
    level.samples = count;
    if (!pcm || count == 0)
        return level;
    double sum = 0;
    double peak = 0;
    for (std::size_t i = 0; i < count; ++i) {
        const double value = static_cast<double>(pcm[i]);
        sum += value * value;
        peak = std::max(peak, std::abs(value));
        if (pcm[i] == 32767 || pcm[i] == -32768)
            ++level.clips;
    }
    level.peak = peak;
    level.rms = std::sqrt(sum / static_cast<double>(count));
    return level;
}

double correlate_at(const std::int16_t* render, std::size_t render_count,
                    const std::int16_t* capture, std::size_t capture_count,
                    std::size_t window, int lag)
{
    double sum_r = 0;
    double sum_c = 0;
    double sum_rr = 0;
    double sum_cc = 0;
    double sum_rc = 0;
    std::size_t n = 0;
    for (std::size_t i = 0; i < window; ++i) {
        const int capture_index = static_cast<int>(i) + lag;
        if (capture_index < 0 || static_cast<std::size_t>(capture_index) >= capture_count || i >= render_count)
            continue;
        const double r = static_cast<double>(render[i]);
        const double c = static_cast<double>(capture[capture_index]);
        sum_r += r;
        sum_c += c;
        sum_rr += r * r;
        sum_cc += c * c;
        sum_rc += r * c;
        ++n;
    }
    if (n < 32)
        return 0;
    const double nf = static_cast<double>(n);
    const double var_r = sum_rr - (sum_r * sum_r) / nf;
    const double var_c = sum_cc - (sum_c * sum_c) / nf;
    if (var_r <= 1 || var_c <= 1)
        return 0;
    return (sum_rc - (sum_r * sum_c) / nf) / std::sqrt(var_r * var_c);
}

LagEstimate estimate_lag(const std::int16_t* render, std::size_t render_count,
                         const std::int16_t* capture, std::size_t capture_count,
                         int max_lag_samples, int step)
{
    LagEstimate best;
    if (!render || !capture || render_count < 32 || capture_count < 32 || step < 1 || max_lag_samples < step)
        return best;

    const std::size_t window = std::min({render_count, capture_count, static_cast<std::size_t>(16000 * 2)});
    const int coarse = std::max(1, step);
    for (int lag = -max_lag_samples; lag <= max_lag_samples; lag += coarse) {
        const double score = correlate_at(render, render_count, capture, capture_count, window, lag);
        if (!best.valid || std::abs(score) > std::abs(best.correlation)) {
            best.valid = true;
            best.lag_samples = lag;
            best.correlation = score;
        }
    }
    if (!best.valid || std::abs(best.correlation) < 1e-3)
        return {};
    const int begin = std::max(-max_lag_samples, best.lag_samples - coarse);
    const int end = std::min(max_lag_samples, best.lag_samples + coarse);
    for (int lag = begin; lag <= end; ++lag) {
        const double score = correlate_at(render, render_count, capture, capture_count, window, lag);
        if (std::abs(score) > std::abs(best.correlation)) {
            best.lag_samples = lag;
            best.correlation = score;
        }
    }
    best.reliable = std::abs(best.correlation) >= 0.5;
    return best;
}

} // namespace pm::audio::aec3
