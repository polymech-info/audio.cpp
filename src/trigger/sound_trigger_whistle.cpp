#include "pm/audio/trigger/sound_trigger_whistle.hpp"

#include "pm/audio/log.hpp"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <string>
#include <utility>
#include <vector>

namespace pm::audio::trigger {
namespace {

constexpr std::uint64_t k_tone_hang_ms = 80;
constexpr std::size_t k_ridge_radius = 10;
constexpr std::size_t k_ridge_guard = 2;
constexpr std::size_t k_acquire_hops = 3;
constexpr std::size_t k_max_ridge_peaks = 8;
constexpr float k_min_prominence = 8.0f;
constexpr float k_hop_glide_hz = 180.0f;
constexpr float k_min_good_fraction = 0.55f;
constexpr std::uint32_t k_min_good_ms = 70;
constexpr float k_prominence_floor = 1.0e-8f;

// Prominence is core power / background power.
// Core: peak ± 2 bins. Background: median power in peak ± 10 bins, core excluded,
// clipped to the whistle band. The median is at least 1e-8 so silence cannot
// explode the ratio. One ridge bin is not used alone; a glide that crosses a
// bin boundary stays in the core.
float ridge_prominence(const SpectrumBins& power, std::size_t peak,
                        std::size_t first, std::size_t last,
                        std::array<float, k_ridge_radius * 2 + 1>& scratch,
                        float& core)
{
    core = 0.0f;
    if (peak < first || peak > last || peak >= power.size())
        return 0.0f;
    const std::size_t core_lo = std::max(first, peak > k_ridge_guard ? peak - k_ridge_guard : first);
    const std::size_t core_hi = std::min(last, peak + k_ridge_guard);
    for (std::size_t k = core_lo; k <= core_hi; ++k)
        core += std::max(power[k], 0.0f);
    const std::size_t left = std::max(first, peak > k_ridge_radius ? peak - k_ridge_radius : first);
    const std::size_t right = std::min(last, peak + k_ridge_radius);
    std::size_t count = 0;
    for (std::size_t k = left; k <= right && count < scratch.size(); ++k) {
        const bool in_core = k + k_ridge_guard >= peak && k <= peak + k_ridge_guard;
        if (in_core)
            continue;
        scratch[count++] = std::max(power[k], 0.0f);
    }
    if (count == 0)
        return 0.0f;
    std::sort(scratch.begin(), scratch.begin() + static_cast<std::ptrdiff_t>(count));
    const float median = std::max(scratch[count / 2], k_prominence_floor);
    return core / median;
}

struct RidgeTracker {
    struct Closed {
        bool event = false;
        bool accepted = false;
        std::uint64_t start_ms = 0;
        std::uint64_t end_ms = 0;
        float mean_hz = 0.0f;
        RidgeReason reason = RidgeReason::none;
    };

    struct Peak {
        std::size_t bin = 0;
        float hz = 0.0f;
        float prominence = 0.0f;
        float core = 0.0f;
        float score = 0.0f;
    };

    RidgeState state = RidgeState::idle;
    bool suppress = false;
    float predicted_hz = 0.0f;
    float pitch_sum = 0.0f;
    std::size_t good_hops = 0;
    std::uint64_t tone_start_ms = 0;
    std::uint64_t last_good_ms = 0;
    std::uint64_t longest_hole_ms = 0;
    std::uint64_t release_ms = 0;
    std::uint32_t hop_ms = 10;
    std::array<float, k_ridge_radius * 2 + 1> scratch{};

    void clear_tone()
    {
        predicted_hz = 0.0f;
        pitch_sum = 0.0f;
        good_hops = 0;
        tone_start_ms = 0;
        last_good_ms = 0;
        longest_hole_ms = 0;
    }

    float fraction() const
    {
        if (good_hops == 0 || last_good_ms < tone_start_ms)
            return 0.0f;
        const auto elapsed = std::max<std::uint64_t>(
            last_good_ms - tone_start_ms, hop_ms);
        const auto good_ms = good_hops * hop_ms;
        return std::min(1.0f, static_cast<float>(good_ms)
            / static_cast<float>(elapsed));
    }

    Closed finish(const WhistleConfig& cfg, RidgeReason forced)
    {
        Closed closed;
        const bool open = state == RidgeState::tracking || state == RidgeState::hole
            || state == RidgeState::acquiring;
        if (!open)
            return closed;
        closed.event = state != RidgeState::acquiring;
        closed.start_ms = tone_start_ms;
        closed.end_ms = last_good_ms;
        closed.mean_hz = good_hops > 0
            ? pitch_sum / static_cast<float>(good_hops) : 0.0f;
        const auto elapsed = closed.end_ms >= closed.start_ms
            ? closed.end_ms - closed.start_ms : 0;
        const auto good_ms = good_hops * hop_ms;
        const float frac = fraction();
        if (forced == RidgeReason::rejected_overlong || elapsed > cfg.max_duration_ms) {
            closed.accepted = false;
            closed.reason = RidgeReason::rejected_overlong;
            suppress = true;
            state = RidgeState::released;
        } else if (elapsed >= cfg.min_duration_ms && elapsed <= cfg.max_duration_ms
                   && good_ms >= k_min_good_ms && frac >= k_min_good_fraction) {
            closed.accepted = true;
            closed.reason = RidgeReason::accepted;
            state = RidgeState::idle;
        } else {
            closed.accepted = false;
            closed.reason = frac < k_min_good_fraction || good_ms < k_min_good_ms
                ? RidgeReason::rejected_sparse
                : RidgeReason::rejected_duration;
            state = RidgeState::idle;
        }
        if (!closed.event)
            closed.reason = RidgeReason::none;
        clear_tone();
        return closed;
    }

    void consider_peak(const SpectrumBins& power, std::size_t bin, std::size_t first,
                       std::size_t last, std::uint32_t sample_rate, float center_hz,
                       float allowed_hz, std::array<Peak, k_max_ridge_peaks>& peaks,
                       std::size_t& count)
    {
        if (bin == 0 || bin >= power.size() || bin < first || bin > last)
            return;
        const float left = bin > 0 ? power[bin - 1] : power[bin];
        const float right = bin + 1 < power.size() ? power[bin + 1] : power[bin];
        if (power[bin] < left || power[bin] < right)
            return;
        const float hz = static_cast<float>(bin) * static_cast<float>(sample_rate)
            / static_cast<float>(k_fft_size);
        const float distance = center_hz > 0.0f ? std::abs(hz - center_hz) : 0.0f;
        if (center_hz > 0.0f && distance > allowed_hz)
            return;
        float core = 0.0f;
        const float prominence = ridge_prominence(power, bin, first, last, scratch, core);
        if (prominence < k_min_prominence)
            return;
        const float closeness = allowed_hz > 0.0f && center_hz > 0.0f
            ? 1.0f - distance / allowed_hz : 1.0f;
        Peak peak;
        peak.bin = bin;
        peak.hz = hz;
        peak.prominence = prominence;
        peak.core = core;
        peak.score = prominence * std::max(closeness, 0.0f);
        if (count < peaks.size()) {
            peaks[count++] = peak;
            return;
        }
        std::size_t worst = 0;
        for (std::size_t i = 1; i < peaks.size(); ++i) {
            if (peaks[i].score < peaks[worst].score)
                worst = i;
        }
        if (peak.score > peaks[worst].score)
            peaks[worst] = peak;
    }

    const Peak* best(const std::array<Peak, k_max_ridge_peaks>& peaks, std::size_t count) const
    {
        const Peak* chosen = nullptr;
        for (std::size_t i = 0; i < count; ++i) {
            if (chosen == nullptr || peaks[i].score > chosen->score)
                chosen = &peaks[i];
        }
        return chosen;
    }

    void accept_hop(const Peak& peak, std::uint64_t frame_ms)
    {
        if (state == RidgeState::idle || state == RidgeState::released
            || tone_start_ms == 0)
            tone_start_ms = frame_ms;
        predicted_hz = peak.hz;
        pitch_sum += peak.hz;
        ++good_hops;
        last_good_ms = frame_ms;
        if (good_hops >= k_acquire_hops && state == RidgeState::acquiring)
            state = RidgeState::tracking;
        else if (state == RidgeState::hole)
            state = RidgeState::tracking;
        else if (state == RidgeState::idle || state == RidgeState::released)
            state = RidgeState::acquiring;
    }

    Closed observe(const SpectrumBins* power, const FrameMetrics& metrics,
                   std::uint64_t frame_ms, std::uint32_t sample_rate,
                   const WhistleConfig& cfg, bool flush, FrameMetrics& out)
    {
        hop_ms = std::max<std::uint32_t>(
            1U, static_cast<std::uint32_t>(k_hop_size * 1000ULL / sample_rate));
        out.predicted_peak_hz = predicted_hz;
        out.good_hop_fraction = fraction();
        out.tracked_peak_hz = 0.0f;
        out.tracked_prominence = 0.0f;
        out.hole_ms = 0;
        const std::size_t first = std::max<std::size_t>(
            1, static_cast<std::size_t>(cfg.min_hz * k_fft_size / sample_rate));
        const std::size_t last = std::min<std::size_t>(
            k_spectrum_bins - 1,
            static_cast<std::size_t>(cfg.max_hz * k_fft_size / sample_rate));
        const bool wide_ok = metrics.active
            && metrics.concentration >= cfg.min_concentration
            && metrics.flatness <= cfg.max_flatness
            && metrics.band_ratio >= cfg.min_band_ratio;
        const bool open = state == RidgeState::tracking || state == RidgeState::hole;
        const bool narrow = open || (state == RidgeState::acquiring && predicted_hz > 0.0f);
        const std::uint64_t since = narrow && frame_ms > last_good_ms
            ? frame_ms - last_good_ms : hop_ms;
        // 350 Hz is the association cap, not an allowed step every hop.
        // A hole adds 1 Hz of reach per millisecond, so a short mask can
        // still find the same ridge.
        const float allowed = narrow
            ? std::min(cfg.max_pitch_jump_hz,
                       k_hop_glide_hz + static_cast<float>(since))
            : cfg.max_pitch_jump_hz;
        std::array<Peak, k_max_ridge_peaks> peaks{};
        std::size_t count = 0;
        if (power && metrics.active && first < last) {
            const float low_hz = narrow && predicted_hz > allowed
                ? predicted_hz - allowed : (narrow ? 0.0f : cfg.min_hz);
            const float high_hz = narrow ? predicted_hz + allowed : cfg.max_hz;
            const std::size_t from = std::max(first, static_cast<std::size_t>(
                std::max(0.0f, low_hz) * static_cast<float>(k_fft_size)
                / static_cast<float>(sample_rate)));
            const std::size_t to = std::min(last, static_cast<std::size_t>(
                std::max(0.0f, high_hz) * static_cast<float>(k_fft_size)
                / static_cast<float>(sample_rate)));
            for (std::size_t bin = std::max<std::size_t>(from, 1); bin <= to; ++bin) {
                consider_peak(*power, bin, first, last, sample_rate,
                              narrow ? predicted_hz : 0.0f, allowed, peaks, count);
            }
        }
        const Peak* chosen = best(peaks, count);
        if (suppress) {
            if (chosen == nullptr) {
                release_ms += hop_ms;
                if (release_ms >= k_tone_hang_ms || flush) {
                    suppress = false;
                    release_ms = 0;
                    state = RidgeState::idle;
                    out.ridge_reason = RidgeReason::none;
                } else {
                    state = RidgeState::released;
                    out.ridge_reason = RidgeReason::waiting_release;
                }
            } else {
                release_ms = 0;
                state = RidgeState::released;
                out.ridge_reason = RidgeReason::waiting_release;
            }
            out.ridge_state = state;
            out.predicted_peak_hz = 0.0f;
            return {};
        }
        const bool gate = open || wide_ok;
        if (chosen != nullptr && gate && !flush
            && open && tone_start_ms > 0 && frame_ms > tone_start_ms
            && frame_ms - tone_start_ms > cfg.max_duration_ms) {
            auto closed = finish(cfg, RidgeReason::rejected_overlong);
            out.ridge_state = state;
            out.ridge_reason = closed.reason;
            out.good_hop_fraction = 0.0f;
            return closed;
        }
        if (chosen != nullptr && gate && !flush) {
            accept_hop(*chosen, frame_ms);
            out.tracked_peak_hz = chosen->hz;
            out.tracked_prominence = chosen->prominence;
            out.hole_ms = 0;
            out.good_hop_fraction = fraction();
            out.ridge_state = state;
            out.ridge_reason = state == RidgeState::acquiring
                ? RidgeReason::acquired : RidgeReason::tracking;
            out.whistle = state == RidgeState::tracking;
            return {};
        }
        if (state == RidgeState::acquiring) {
            clear_tone();
            state = RidgeState::idle;
            out.ridge_state = state;
            out.ridge_reason = RidgeReason::none;
            out.good_hop_fraction = 0.0f;
            return {};
        }
        if (state == RidgeState::tracking || state == RidgeState::hole) {
            const std::uint64_t gap = frame_ms > last_good_ms
                ? frame_ms - last_good_ms : hop_ms;
            if (gap > longest_hole_ms)
                longest_hole_ms = gap;
            out.hole_ms = static_cast<std::uint32_t>(std::min<std::uint64_t>(gap, 100000));
            out.good_hop_fraction = fraction();
            out.predicted_peak_hz = predicted_hz;
            const auto hang = std::min<std::uint64_t>(k_tone_hang_ms, cfg.min_gap_ms / 2);
            if (!flush && gap <= hang) {
                state = RidgeState::hole;
                out.ridge_state = state;
                out.ridge_reason = RidgeReason::hole;
                return {};
            }
            auto closed = finish(cfg, RidgeReason::none);
            out.ridge_state = state;
            out.ridge_reason = closed.reason;
            out.good_hop_fraction = 0.0f;
            out.hole_ms = 0;
            return closed;
        }
        out.ridge_state = RidgeState::idle;
        out.ridge_reason = RidgeReason::none;
        return {};
    }
};

} // namespace

void add_spectral_metrics(const SpectrumBins& power, std::uint32_t sample_rate,
                          float min_hz, float max_hz, FrameMetrics& m)
{
    const std::size_t first = std::max<std::size_t>(
        1, static_cast<std::size_t>(min_hz * k_fft_size / sample_rate));
    const std::size_t last = std::min<std::size_t>(
        power.size() - 1,
        static_cast<std::size_t>(max_hz * k_fft_size / sample_rate));
    if (first > last)
        return;
    float band_sum = 0.0f;
    float peak = 0.0f;
    std::size_t peak_bin = first;
    double log_sum = 0.0;
    for (std::size_t k = first; k <= last; ++k) {
        const float v = std::sqrt(std::max(power[k], 0.0f));
        const float bounded = std::max(v, 1e-12f);
        band_sum += bounded;
        log_sum += std::log(bounded);
        if (bounded > peak) {
            peak = bounded;
            peak_bin = k;
        }
    }
    const std::size_t count = last - first + 1;
    const float arithmetic = band_sum / static_cast<float>(count);
    m.flatness = arithmetic > 0.0f
        ? static_cast<float>(std::exp(log_sum / static_cast<double>(count))) / arithmetic
        : 1.0f;
    float local = 0.0f;
    const std::size_t lo = std::max(first, peak_bin > 2 ? peak_bin - 2 : 0);
    const std::size_t hi = std::min(last, peak_bin + 2);
    for (std::size_t k = lo; k <= hi; ++k)
        local += std::sqrt(std::max(power[k], 0.0f));
    m.concentration = band_sum > 0.0f ? std::min(1.0f, local / band_sum) : 0.0f;
    m.dominant_hz = static_cast<float>(peak_bin) * sample_rate
        / static_cast<float>(k_fft_size);
    float core = 0.0f;
    std::array<float, k_ridge_radius * 2 + 1> scratch{};
    m.prominence = ridge_prominence(power, peak_bin, first, last, scratch, core);
    float low_sum = 0.0f;
    for (std::size_t k = 1; k < first; ++k)
        low_sum += std::sqrt(std::max(power[k], 0.0f));
    m.band_ratio = band_sum / std::max(low_sum, 1e-12f);
}


struct WhistleTracker::Impl {
    RidgeTracker ridge;
    std::uint64_t first_start_ms = 0;
    std::uint64_t first_end_ms = 0;
    std::uint64_t cooldown_until_ms = 0;
    float first_pitch = 0.0f;
    bool first_waiting = false;
    bool second_attempt = false;

    void run(const FrameMetrics& base,
             const SpectrumBins* frame_spectrum,
             std::uint32_t sample_rate,
             std::uint64_t frame_ms,
             const WhistleConfig& cfg,
             const std::string& trigger_id,
             bool flush,
             std::vector<Detection>& detections)
    {
        if (first_waiting && !second_attempt
            && frame_ms > first_end_ms
            && frame_ms - first_end_ms > cfg.max_gap_ms) {
            if (pm::audio::log::trace_enabled()) {
                pm::audio::log::trace("[sound-trigger] whistle pair expired id="
                    + trigger_id);
            }
            first_waiting = false;
        }
        FrameMetrics m = base;
        const bool need_wide = ridge.state == RidgeState::idle
            || ridge.state == RidgeState::acquiring;
        if (m.active && frame_spectrum && need_wide)
            add_spectral_metrics(*frame_spectrum, sample_rate,
                                 cfg.min_hz, cfg.max_hz, m);
        const auto closed = ridge.observe(
            m.active ? frame_spectrum : nullptr, m, frame_ms, sample_rate,
            cfg, flush, m);
        second_attempt = first_waiting
            && (ridge.state == RidgeState::tracking
                || ridge.state == RidgeState::hole);
        if (!closed.event)
            return;
        const auto end_ms = closed.end_ms;
        const auto duration = end_ms >= closed.start_ms
            ? end_ms - closed.start_ms : 0;
        if (pm::audio::log::debug_enabled()) {
            pm::audio::log::debug("[sound-trigger] whistle event id="
                + trigger_id
                + " duration_ms=" + std::to_string(duration)
                + " pitch_hz=" + std::to_string(closed.mean_hz)
                + " reason=" + ridge_reason_name(closed.reason)
                + " accepted=" + (closed.accepted ? "1" : "0"));
        }
        if (!closed.accepted || end_ms < cooldown_until_ms)
            return;
        if (!first_waiting) {
            first_waiting = true;
            first_start_ms = closed.start_ms;
            first_end_ms = end_ms;
            first_pitch = closed.mean_hz;
            return;
        }
        const auto gap = closed.start_ms > first_end_ms
            ? closed.start_ms - first_end_ms : 0;
        if (gap < cfg.min_gap_ms || gap > cfg.max_gap_ms
            || std::abs(closed.mean_hz - first_pitch) > cfg.max_pitch_jump_hz) {
            first_start_ms = closed.start_ms;
            first_end_ms = end_ms;
            first_pitch = closed.mean_hz;
            return;
        }
        Detection d;
        d.operation_id = next_operation_id();
        d.trigger_id = trigger_id;
        d.score_kind = "whistle-rule";
        const float pitch_margin = 1.0f
            - std::min(1.0f, std::abs(closed.mean_hz - first_pitch)
                                 / cfg.max_pitch_jump_hz);
        d.score = std::max(0.0f, pitch_margin);
        d.start_time_ms = first_start_ms;
        d.end_time_ms = end_ms;
        detections.push_back(std::move(d));
        first_waiting = false;
        second_attempt = false;
        cooldown_until_ms = frame_ms + cfg.cooldown_ms;
        pm::audio::log::info("[sound-trigger] double whistle matched id="
            + trigger_id + " gap_ms=" + std::to_string(gap));
    }
};

WhistleTracker::WhistleTracker()
    : impl_(std::make_unique<Impl>())
{
}

WhistleTracker::~WhistleTracker() = default;
WhistleTracker::WhistleTracker(WhistleTracker&&) noexcept = default;
WhistleTracker& WhistleTracker::operator=(WhistleTracker&&) noexcept = default;

void WhistleTracker::observe(const FrameMetrics& base,
                             const SpectrumBins* spectrum,
                             std::uint32_t sample_rate,
                             std::uint64_t frame_ms,
                             const WhistleConfig& cfg,
                             const std::string& trigger_id,
                             bool flush,
                             std::vector<Detection>& detections)
{
    impl_->run(base, spectrum, sample_rate, frame_ms, cfg, trigger_id,
               flush, detections);
}

void WhistleTracker::inspect(const SpectrumBins* spectrum,
                             FrameMetrics& frame,
                             std::uint32_t sample_rate,
                             std::uint64_t frame_ms,
                             const WhistleConfig& cfg,
                             bool flush)
{
    impl_->ridge.observe(spectrum, frame, frame_ms, sample_rate, cfg, flush, frame);
}

} // namespace pm::audio::trigger
