#pragma once

#include "pm/audio/constants.hpp"

#include <atomic>
#include <condition_variable>
#include <cstddef>
#include <cstdint>
#include <mutex>

namespace pm::audio {

// Single-producer, single-consumer ring of bytes.
// One thread calls push or try_push. A different thread calls pop or peek.
// push waits while the ring is full. A capture callback must use try_push,
// which writes what fits and returns without waiting or allocating.
struct AudioRingBuffer {
    static constexpr std::size_t k_cap = pm::audio::constants::k_ring_cap;

    uint8_t buf[k_cap];
    std::atomic<size_t> wpos{0};
    std::atomic<size_t> rpos{0};
    std::atomic<bool> done{false};
    std::atomic<bool> cancelled{false};

    std::condition_variable cv;
    std::mutex mu;

    size_t readable() const noexcept
    {
        return wpos.load(std::memory_order_acquire) - rpos.load(std::memory_order_acquire);
    }

    bool push(const void* src, size_t len);
    // Bytes accepted. Zero when cancelled, empty, or the ring has no space.
    size_t try_push(const void* src, size_t len) noexcept;
    size_t pop(void* dst, size_t len) noexcept;
    size_t peek(void* dst, size_t len) const noexcept;
    void wait_for(size_t n);
    void mark_done();
    void cancel();

    bool exhausted() const noexcept
    {
        return done.load(std::memory_order_acquire) && readable() == 0;
    }
};

} // namespace pm::audio
