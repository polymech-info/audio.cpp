#include "pm/audio/log.hpp"

#include <mutex>

namespace pm::audio::log {
namespace {

std::mutex g_mu;
EnabledFn g_enabled = nullptr;
SinkFn g_sink = nullptr;

bool default_enabled(Level level)
{
    return level == Level::info || level == Level::warn;
}

} // namespace

void set_enabled(EnabledFn fn)
{
    std::lock_guard<std::mutex> lock(g_mu);
    g_enabled = fn;
}

void set_sink(SinkFn fn)
{
    std::lock_guard<std::mutex> lock(g_mu);
    g_sink = fn;
}

bool trace_enabled()
{
    std::lock_guard<std::mutex> lock(g_mu);
    return g_enabled ? g_enabled(Level::trace) : false;
}

bool debug_enabled()
{
    std::lock_guard<std::mutex> lock(g_mu);
    return g_enabled ? g_enabled(Level::debug) : false;
}

namespace {

void write(Level level, const std::string& message)
{
    SinkFn sink = nullptr;
    EnabledFn enabled = nullptr;
    {
        std::lock_guard<std::mutex> lock(g_mu);
        sink = g_sink;
        enabled = g_enabled;
    }
    if (enabled) {
        if (!enabled(level))
            return;
    } else if (!default_enabled(level)) {
        return;
    }
    if (sink)
        sink(level, message);
}

} // namespace

void trace(const std::string& message) { write(Level::trace, message); }
void debug(const std::string& message) { write(Level::debug, message); }
void info(const std::string& message) { write(Level::info, message); }
void warn(const std::string& message) { write(Level::warn, message); }

} // namespace pm::audio::log
