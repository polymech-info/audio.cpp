#include "pm/audio/ring.hpp"

#include "pm/audio/log.hpp"

#include <algorithm>
#include <chrono>
#include <cstring>

namespace pm::audio {

bool AudioRingBuffer::push(const void* src, size_t len)
{
    const auto* p = static_cast<const uint8_t*>(src);
    size_t written = 0;
    while (written < len) {
        if (cancelled.load(std::memory_order_acquire))
            return false;

        const size_t space = k_cap - readable();
        if (space == 0) {
            std::unique_lock<std::mutex> lk(mu);
            cv.wait_for(lk, std::chrono::milliseconds(constants::k_ring_push_wait_ms), [&] {
                return k_cap - readable() > 0 || cancelled.load();
            });
            continue;
        }

        const size_t chunk = std::min(len - written, space);
        const size_t wp = wpos.load(std::memory_order_relaxed) % k_cap;
        const size_t tail = k_cap - wp;
        if (chunk <= tail) {
            std::memcpy(buf + wp, p + written, chunk);
        } else {
            std::memcpy(buf + wp, p + written, tail);
            std::memcpy(buf, p + written + tail, chunk - tail);
        }
        wpos.fetch_add(chunk, std::memory_order_release);
        written += chunk;
        cv.notify_all();
    }
    return true;
}

size_t AudioRingBuffer::pop(void* dst, size_t len) noexcept
{
    const size_t avail = readable();
    if (avail == 0)
        return 0;
    const size_t chunk = std::min(len, avail);
    const size_t rp = rpos.load(std::memory_order_relaxed) % k_cap;
    const size_t tail = k_cap - rp;
    auto* out = static_cast<uint8_t*>(dst);
    if (chunk <= tail) {
        std::memcpy(out, buf + rp, chunk);
    } else {
        std::memcpy(out, buf + rp, tail);
        std::memcpy(out + tail, buf, chunk - tail);
    }
    rpos.fetch_add(chunk, std::memory_order_release);
    cv.notify_all();
    return chunk;
}

size_t AudioRingBuffer::peek(void* dst, size_t len) const noexcept
{
    const size_t avail = readable();
    if (avail == 0)
        return 0;
    const size_t chunk = std::min(len, avail);
    const size_t rp = rpos.load(std::memory_order_relaxed) % k_cap;
    const size_t tail = k_cap - rp;
    auto* out = static_cast<uint8_t*>(dst);
    if (chunk <= tail) {
        std::memcpy(out, buf + rp, chunk);
    } else {
        std::memcpy(out, buf + rp, tail);
        std::memcpy(out + tail, buf, chunk - tail);
    }
    return chunk;
}

void AudioRingBuffer::wait_for(size_t n)
{
    std::unique_lock<std::mutex> lk(mu);
    cv.wait(lk, [&] {
        return readable() >= n || done.load() || cancelled.load();
    });
}

void AudioRingBuffer::mark_done()
{
    done.store(true, std::memory_order_release);
    cv.notify_all();
}

void AudioRingBuffer::cancel()
{
    cancelled.store(true, std::memory_order_release);
    cv.notify_all();
    log::trace("[cancel] AudioRingBuffer::cancel");
}

} // namespace pm::audio
