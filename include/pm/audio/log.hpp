#pragma once

#include <string>

namespace pm::audio::log {

bool trace_enabled();
bool debug_enabled();
void trace(const std::string& message);
void debug(const std::string& message);
void info(const std::string& message);
void warn(const std::string& message);

enum class Level { trace, debug, info, warn };

using EnabledFn = bool (*)(Level);
using SinkFn = void (*)(Level, const std::string&);

void set_enabled(EnabledFn fn);
void set_sink(SinkFn fn);

} // namespace pm::audio::log
