#include "pm/audio/mic_activity.hpp"

#include <atomic>
#include <memory>
#include <mutex>

namespace pm::audio {

namespace {

std::atomic<int> g_active{0};
std::mutex g_mu;
std::shared_ptr<MicStateFn> g_on_state;
std::shared_ptr<MicLevelFn> g_on_level;

} // namespace

void set_mic_activity_listener(MicStateFn on_state, MicLevelFn on_level)
{
    std::lock_guard<std::mutex> lk(g_mu);
    g_on_state = on_state ? std::make_shared<MicStateFn>(std::move(on_state)) : nullptr;
    g_on_level = on_level ? std::make_shared<MicLevelFn>(std::move(on_level)) : nullptr;
}

void mic_notify_level(double level)
{
    std::shared_ptr<MicLevelFn> fn;
    { std::lock_guard<std::mutex> lk(g_mu); fn = g_on_level; }
    if (fn && *fn) (*fn)(level);
}

MicActivityScope::MicActivityScope()
{
    const int prev = g_active.fetch_add(1, std::memory_order_acq_rel);
    if (prev == 0) {
        std::shared_ptr<MicStateFn> fn;
        { std::lock_guard<std::mutex> lk(g_mu); fn = g_on_state; }
        if (fn && *fn) (*fn)(true);
    }
}

MicActivityScope::~MicActivityScope()
{
    const int remaining = g_active.fetch_sub(1, std::memory_order_acq_rel) - 1;
    if (remaining <= 0) {
        g_active.store(0, std::memory_order_release);
        std::shared_ptr<MicStateFn> fn;
        { std::lock_guard<std::mutex> lk(g_mu); fn = g_on_state; }
        if (fn && *fn) (*fn)(false);
    }
}

} // namespace pm::audio
