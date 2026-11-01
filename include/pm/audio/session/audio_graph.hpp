#pragma once

// PCM graph pieces that do not open a device.
// A capture callback may only copy into SpscRing. Filters and sinks run on a
// worker. One slow sink must not stall the others: each sink has its own queue.

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <vector>

namespace pm::audio::graph {

struct PcmFormat {
    std::uint32_t rate = 0;
    std::uint16_t channels = 0;
};

inline bool formats_equal(PcmFormat a, PcmFormat b) noexcept
{
    return a.rate != 0 && a.channels != 0 && a.rate == b.rate && a.channels == b.channels;
}

struct ConnectResult {
    bool ok = false;
    const char* reason = "";
};

ConnectResult connect_formats(PcmFormat produced, PcmFormat accepted) noexcept;

// Single-producer, single-consumer ring of s16 samples.
// try_write never blocks. Samples that do not fit are counted as dropped
// and are not stored (the callback must not wait).
class SpscRing {
public:
    explicit SpscRing(std::size_t capacity_samples);
    SpscRing(const SpscRing&) = delete;
    SpscRing& operator=(const SpscRing&) = delete;

    std::size_t capacity() const noexcept { return usable_; }
    // Writes what fits. Does not count the remainder; the caller retries or drops it.
    std::size_t try_write(const std::int16_t* src, std::size_t n) noexcept;
    // Capture-callback helper: write what fits and count the rest as dropped.
    std::size_t write_or_drop(const std::int16_t* src, std::size_t n) noexcept;
    std::size_t try_read(std::int16_t* dst, std::size_t n) noexcept;
    std::size_t size() const noexcept;
    std::uint64_t dropped() const noexcept { return dropped_.load(std::memory_order_acquire); }

private:
    std::vector<std::int16_t> buf_;
    std::size_t mask_ = 0;
    std::size_t usable_ = 0;
    std::atomic<std::size_t> write_{0};
    std::atomic<std::size_t> read_{0};
    std::atomic<std::uint64_t> dropped_{0};
};

enum class Pressure : uint8_t { DropOldest, Block };

// Worker-side queue for one sink. Not thread-safe; the worker owns it.
class FrameQueue {
public:
    FrameQueue(std::size_t capacity_samples, Pressure pressure);

    // DropOldest keeps the newest samples and returns true.
    // Block writes nothing and returns false when the chunk does not fit.
    bool push(const std::int16_t* src, std::size_t n);
    std::size_t pop(std::int16_t* dst, std::size_t n);
    std::size_t size() const noexcept { return buf_.size(); }
    std::size_t capacity() const noexcept { return capacity_; }
    std::uint64_t dropped() const noexcept { return dropped_; }
    Pressure pressure() const noexcept { return pressure_; }

private:
    std::size_t capacity_ = 0;
    Pressure pressure_ = Pressure::DropOldest;
    std::vector<std::int16_t> buf_;
    std::uint64_t dropped_ = 0;
};

struct TeePush {
    bool all_accepted = true;
    unsigned blocked = 0;
};

// Copies one chunk to every sink queue. A blocked sink does not stop the others.
class Tee {
public:
    std::size_t add_sink(std::size_t capacity_samples, Pressure pressure);
    TeePush push(const std::int16_t* src, std::size_t n);
    FrameQueue& sink(std::size_t index) { return sinks_.at(index); }
    const FrameQueue& sink(std::size_t index) const { return sinks_.at(index); }
    std::size_t sink_count() const noexcept { return sinks_.size(); }

private:
    std::vector<FrameQueue> sinks_;
};

class ReplaySource {
public:
    ReplaySource(std::vector<std::int16_t> pcm, PcmFormat format, std::size_t hop_frames);
    PcmFormat format() const noexcept { return format_; }
    std::size_t remaining() const noexcept { return pcm_.size() - pos_; }
    // False when the source is exhausted. The last pull may be shorter than hop.
    bool pull(std::vector<std::int16_t>& out);

private:
    std::vector<std::int16_t> pcm_;
    PcmFormat format_{};
    std::size_t hop_ = 0;
    std::size_t pos_ = 0;
};

// Linear gain with saturation. Worker thread only.
void apply_gain(const std::int16_t* in, std::size_t n, float gain, std::vector<std::int16_t>& out);

} // namespace pm::audio::graph
