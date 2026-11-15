#pragma once
//
// Process-wide microphone activity notifications.
//
// Any code that opens an AudioInput should:
//   1. Construct a MicActivityScope for the lifetime of the capture.
//   2. Call mic_notify_level(…) from the on_pcm callback for UI meters.
//
// The host registers one listener via set_mic_activity_listener. Callbacks run
// on arbitrary threads and must be quick (e.g. post to the UI thread).
//
// Process-global hook: the host compiles mic_activity.cpp once (not in the
// standalone pm-audio-lib glob under a parent build).

#include <functional>

namespace pm::audio {

using MicStateFn = std::function<void(bool active)>;  // true = first start, false = last stop
using MicLevelFn = std::function<void(double level)>; // level ∈ [0, 1]

void set_mic_activity_listener(MicStateFn on_state, MicLevelFn on_level);
void mic_notify_level(double level);

class MicActivityScope {
public:
    MicActivityScope();
    ~MicActivityScope();
    MicActivityScope(const MicActivityScope&) = delete;
    MicActivityScope& operator=(const MicActivityScope&) = delete;
};

} // namespace pm::audio
