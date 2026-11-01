#include "pm/audio/session/audio_graph.hpp"

#include <algorithm>
#include <cstddef>
#include <cstring>

namespace pm::audio::graph {
namespace {

std::size_t round_pow2(std::size_t n)
{
    std::size_t c = 1;
    while (c < n) {
        if (c > (static_cast<std::size_t>(-1) >> 1))
            return c;
        c <<= 1;
    }
    return c;
}

std::int16_t sat(float s)
{
    if (s > 32767.f)
        return 32767;
    if (s < -32768.f)
        return -32768;
    return static_cast<std::int16_t>(s);
}

} // namespace

ConnectResult connect_formats(PcmFormat produced, PcmFormat accepted) noexcept
{
    ConnectResult out;
    if (produced.rate == 0 || produced.channels == 0 || accepted.rate == 0 || accepted.channels == 0) {
        out.reason = "format is unset";
        return out;
    }
    if (!formats_equal(produced, accepted)) {
        out.reason = "format mismatch";
        return out;
    }
    out.ok = true;
    out.reason = "formats match";
    return out;
}

SpscRing::SpscRing(std::size_t capacity_samples)
{
    if (capacity_samples < 1)
        capacity_samples = 1;
    const std::size_t slots = round_pow2(capacity_samples + 1);
    buf_.assign(slots, 0);
    mask_ = slots - 1;
    usable_ = slots - 1;
}

std::size_t SpscRing::try_write(const std::int16_t* src, std::size_t n) noexcept
{
    if (!src || n == 0)
        return 0;
    const std::size_t w = write_.load(std::memory_order_relaxed);
    const std::size_t r = read_.load(std::memory_order_acquire);
    const std::size_t used = w - r;
    const std::size_t free = usable_ - used;
    const std::size_t take = n < free ? n : free;
    for (std::size_t i = 0; i < take; ++i)
        buf_[(w + i) & mask_] = src[i];
    write_.store(w + take, std::memory_order_release);
    return take;
}

std::size_t SpscRing::write_or_drop(const std::int16_t* src, std::size_t n) noexcept
{
    const std::size_t wrote = try_write(src, n);
    if (wrote < n)
        dropped_.fetch_add(n - wrote, std::memory_order_relaxed);
    return wrote;
}

std::size_t SpscRing::try_read(std::int16_t* dst, std::size_t n) noexcept
{
    if (!dst || n == 0)
        return 0;
    const std::size_t r = read_.load(std::memory_order_relaxed);
    const std::size_t w = write_.load(std::memory_order_acquire);
    const std::size_t have = w - r;
    const std::size_t take = n < have ? n : have;
    for (std::size_t i = 0; i < take; ++i)
        dst[i] = buf_[(r + i) & mask_];
    read_.store(r + take, std::memory_order_release);
    return take;
}

std::size_t SpscRing::size() const noexcept
{
    const std::size_t w = write_.load(std::memory_order_acquire);
    const std::size_t r = read_.load(std::memory_order_acquire);
    return w - r;
}

FrameQueue::FrameQueue(std::size_t capacity_samples, Pressure pressure)
    : capacity_(capacity_samples < 1 ? 1 : capacity_samples)
    , pressure_(pressure)
{
    buf_.reserve(capacity_);
}

bool FrameQueue::push(const std::int16_t* src, std::size_t n)
{
    if (!src || n == 0)
        return true;
    if (pressure_ == Pressure::Block) {
        if (buf_.size() + n > capacity_)
            return false;
        buf_.insert(buf_.end(), src, src + n);
        return true;
    }
    if (n >= capacity_) {
        dropped_ += buf_.size() + (n - capacity_);
        buf_.assign(src + (n - capacity_), src + n);
        return true;
    }
    buf_.insert(buf_.end(), src, src + n);
    if (buf_.size() > capacity_) {
        const std::size_t extra = buf_.size() - capacity_;
        dropped_ += extra;
        buf_.erase(buf_.begin(), buf_.begin() + static_cast<std::ptrdiff_t>(extra));
    }
    return true;
}

std::size_t FrameQueue::pop(std::int16_t* dst, std::size_t n)
{
    if (!dst || n == 0 || buf_.empty())
        return 0;
    const std::size_t take = std::min(n, buf_.size());
    std::memcpy(dst, buf_.data(), take * sizeof(std::int16_t));
    buf_.erase(buf_.begin(), buf_.begin() + static_cast<std::ptrdiff_t>(take));
    return take;
}

std::size_t Tee::add_sink(std::size_t capacity_samples, Pressure pressure)
{
    sinks_.emplace_back(capacity_samples, pressure);
    return sinks_.size() - 1;
}

TeePush Tee::push(const std::int16_t* src, std::size_t n)
{
    TeePush result;
    for (FrameQueue& q : sinks_) {
        if (!q.push(src, n)) {
            result.all_accepted = false;
            ++result.blocked;
        }
    }
    return result;
}

ReplaySource::ReplaySource(std::vector<std::int16_t> pcm, PcmFormat format, std::size_t hop_frames)
    : pcm_(std::move(pcm))
    , format_(format)
    , hop_(hop_frames < 1 ? 1 : hop_frames)
{
}

bool ReplaySource::pull(std::vector<std::int16_t>& out)
{
    if (pos_ >= pcm_.size()) {
        out.clear();
        return false;
    }
    const std::size_t n = std::min(hop_, pcm_.size() - pos_);
    out.assign(pcm_.begin() + static_cast<std::ptrdiff_t>(pos_),
               pcm_.begin() + static_cast<std::ptrdiff_t>(pos_ + n));
    pos_ += n;
    return true;
}

void apply_gain(const std::int16_t* in, std::size_t n, float gain, std::vector<std::int16_t>& out)
{
    out.resize(n);
    if (!in || n == 0)
        return;
    for (std::size_t i = 0; i < n; ++i)
        out[i] = sat(static_cast<float>(in[i]) * gain);
}

} // namespace pm::audio::graph
