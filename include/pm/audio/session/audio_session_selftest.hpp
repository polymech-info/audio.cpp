#pragma once

#include <string>
#include <string_view>

namespace pm::audio::session {

struct SelfTestReport {
    bool ok = false;
    std::string json;
};

// suite: all, lease, route, event, graph. Unknown suite returns ok=false.
SelfTestReport run_selftest(std::string_view suite);

} // namespace pm::audio::session
