#pragma once

// Trial clock map for speech and the phone mic. Pass-through and speakers do not use it.
// Nothing here runs in an audio callback. A derivative controller is not part of this trial.
// One estimate must not step a playing device. The desktop is the master when a caller opts in.

#include <algorithm>
#include <cstdint>

namespace pm::audio {

enum class ClockTrialUse { None, Speech, Mic };

inline bool clock_trial_enabled(ClockTrialUse use)
{
    return use == ClockTrialUse::Speech || use == ClockTrialUse::Mic;
}

inline constexpr std::uint32_t k_clock_bus_rate = 48000;
inline constexpr std::uint32_t k_clock_frame_samples = 480;
inline constexpr int k_clock_window = 16;
inline constexpr std::int64_t k_clock_stall_ns = 500000000LL;

struct AudioFrameHeader {
    std::uint64_t session_id = 0;
    std::uint64_t sample_index = 0;
    std::uint32_t frame_count = 0;
    std::uint32_t epoch = 0;
};

class SessionTimeline {
public:
    void reset(std::uint64_t session_id)
    {
        session_id_ = session_id;
        sample_index_ = 0;
        ++epoch_;
    }

    AudioFrameHeader stamp(std::uint32_t frame_count)
    {
        AudioFrameHeader header;
        header.session_id = session_id_;
        header.sample_index = sample_index_;
        header.frame_count = frame_count;
        header.epoch = epoch_;
        sample_index_ += frame_count;
        return header;
    }

    std::uint32_t epoch() const { return epoch_; }
    std::uint64_t sample_index() const { return sample_index_; }

private:
    std::uint64_t session_id_ = 0;
    std::uint64_t sample_index_ = 0;
    std::uint32_t epoch_ = 0;
};

struct ClockExchange {
    std::int64_t t1_ns = 0;
    std::int64_t t2_ns = 0;
    std::int64_t t3_ns = 0;
    std::int64_t t4_ns = 0;
};

class ClockSync {
public:
    struct Row {
        std::int64_t local_ns = 0;
        std::int64_t offset_ns = 0;
        std::int64_t rtt_ns = 0;
    };

    void reset()
    {
        count_ = 0;
        origin_ns_ = 0;
        offset0_ = 0;
        drift_ppm_ = 0;
        uncertainty_ms_ = 0;
    }

    bool observe(const ClockExchange& sample)
    {
        if (sample.t4_ns < sample.t1_ns || sample.t3_ns < sample.t2_ns)
            return false;
        const std::int64_t rtt = (sample.t4_ns - sample.t1_ns) - (sample.t3_ns - sample.t2_ns);
        if (rtt < 0 || rtt > k_clock_stall_ns)
            return false;
        if (count_ >= 4) {
            std::int64_t best = rows_[0].rtt_ns;
            for (int i = 1; i < count_; ++i)
                best = std::min(best, rows_[i].rtt_ns);
            if (rtt > best * 4 && rtt > best + 5000000LL)
                return false;
        }
        const std::int64_t offset =
            ((sample.t2_ns - sample.t1_ns) + (sample.t3_ns - sample.t4_ns)) / 2;
        const std::int64_t local = sample.t1_ns + (sample.t4_ns - sample.t1_ns) / 2;
        if (count_ == 0)
            origin_ns_ = local;
        if (count_ < k_clock_window) {
            rows_[count_++] = Row{local, offset, rtt};
        } else {
            int worst = 0;
            for (int i = 1; i < count_; ++i) {
                if (rows_[i].rtt_ns > rows_[worst].rtt_ns)
                    worst = i;
            }
            if (rtt >= rows_[worst].rtt_ns)
                return false;
            rows_[worst] = Row{local, offset, rtt};
        }
        refit();
        return true;
    }

    std::int64_t toMasterTime(std::int64_t local_ns) const
    {
        if (count_ <= 0)
            return local_ns;
        const double x = static_cast<double>(local_ns - origin_ns_);
        const double off = offset0_ + (drift_ppm_ / 1000000.0) * x;
        return local_ns + static_cast<std::int64_t>(off);
    }

    std::int64_t toLocalTime(std::int64_t master_ns) const
    {
        if (count_ <= 0)
            return master_ns;
        const std::int64_t guess = master_ns - static_cast<std::int64_t>(offset0_);
        const double x = static_cast<double>(guess - origin_ns_);
        const double off = offset0_ + (drift_ppm_ / 1000000.0) * x;
        return master_ns - static_cast<std::int64_t>(off);
    }

    double driftPpm() const { return drift_ppm_; }
    double uncertaintyMs() const { return uncertainty_ms_; }
    int samples() const { return count_; }
    bool ready() const { return count_ > 0; }

private:
    void refit()
    {
        if (count_ <= 0) {
            offset0_ = 0;
            drift_ppm_ = 0;
            uncertainty_ms_ = 0;
            return;
        }
        std::int64_t best = rows_[0].rtt_ns;
        std::int64_t worst = rows_[0].rtt_ns;
        std::int64_t earliest = rows_[0].local_ns;
        std::int64_t latest = rows_[0].local_ns;
        double sy = 0;
        for (int i = 0; i < count_; ++i) {
            best = std::min(best, rows_[i].rtt_ns);
            worst = std::max(worst, rows_[i].rtt_ns);
            earliest = std::min(earliest, rows_[i].local_ns);
            latest = std::max(latest, rows_[i].local_ns);
            sy += static_cast<double>(rows_[i].offset_ns);
        }
        const std::int64_t spread = std::max(worst / 2, (worst - best) / 2);
        uncertainty_ms_ = static_cast<double>(spread) / 1000000.0;
        const double mean_off = sy / static_cast<double>(count_);
        drift_ppm_ = 0;
        offset0_ = mean_off;
        if (count_ < 2 || latest - earliest < 1000000000LL)
            return;
        double sx = 0;
        double sxx = 0;
        double sxy = 0;
        sy = 0;
        for (int i = 0; i < count_; ++i) {
            const double x = static_cast<double>(rows_[i].local_ns - origin_ns_);
            const double y = static_cast<double>(rows_[i].offset_ns);
            sx += x;
            sy += y;
            sxx += x * x;
            sxy += x * y;
        }
        const double n = static_cast<double>(count_);
        const double denom = n * sxx - sx * sx;
        if (denom == 0)
            return;
        const double slope = (n * sxy - sx * sy) / denom;
        drift_ppm_ = slope * 1000000.0;
        offset0_ = (sy / n) - slope * (sx / n);
    }

    Row rows_[k_clock_window]{};
    int count_ = 0;
    std::int64_t origin_ns_ = 0;
    double offset0_ = 0;
    double drift_ppm_ = 0;
    double uncertainty_ms_ = 0;
};

} // namespace pm::audio
