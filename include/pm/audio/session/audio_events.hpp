#pragma once

// One event envelope for the phone, JNI, and the desktop LAN SSE stream.
// Method names are the ones the chat panel already calls.

#include "pm/audio/session/audio_lease.hpp"

#include <cstdint>
#include <string>
#include <string_view>

namespace pm::audio::session {

enum class AudioMethod : uint8_t {
    Ready,
    TextDelta,
    TranscriptDelta,
    UserTranscript,
    ResponseDone,
    Error,
    AuthError,
    NetworkError,
    AppendText,
    Done,
    AgentStarted,
    AgentDone,
    ConsentRequest,
    ConsentDismiss,
    AskUser,
    Level,
    DeviceLost,
    WalkieState,
    FloorGrant,
    FloorDeny,
    FloorReleased,
};

const char* audio_method_name(AudioMethod method) noexcept;

struct AudioEnvelope {
    std::string session_id;
    SessionKind kind = SessionKind::None;
    std::uint64_t seq = 0;
    std::int64_t ts_ms = 0;
    std::string participant_id;
    AudioMethod method = AudioMethod::Ready;
    std::string args_json = "[]";  // a JSON value; empty becomes []
};

// Compact JSON object. args_json is embedded as-is when it looks like JSON,
// otherwise the args field is [] and ok() is not tracked: invalid args become [].
std::string envelope_json(const AudioEnvelope& event);

} // namespace pm::audio::session
