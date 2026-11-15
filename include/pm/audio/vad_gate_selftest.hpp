#pragma once

#include <string>

namespace pm::audio {

struct VadGateSelfTestReport {
    bool ok = false;
    std::string detail;
};

// Conversational gate: two utterances separated by sub-5 s silence; mic open
// while playback telemetry says the speaker is still idle (pre-buffer).
VadGateSelfTestReport run_vad_gate_selftest();

} // namespace pm::audio