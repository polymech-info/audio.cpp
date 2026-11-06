#pragma once

// One audio lease per process. Callers ask preempt() before opening a device
// or changing the communication route. This is the policy table, not device I/O.

#include <cstdint>
#include <string_view>

namespace pm::audio::session {

enum class SessionKind : uint8_t {
    None = 0,
    Dictation,
    ReadAloud,
    AiConversation,
    WalkieListen,
    WalkieTransmit,
    GroupVoice,
    LanRemote,
    PhoneCall,
};

enum class LeaseAction : uint8_t {
    Allow,    // incoming may start; stop the holder only when it is the same channel upgrade
    Deny,
    Preempt,  // stop the holder, then start incoming
};

struct LeaseDecision {
    LeaseAction action = LeaseAction::Deny;
    const char* reason = "";
};

const char* session_kind_name(SessionKind kind) noexcept;
SessionKind parse_session_kind(std::string_view token) noexcept; // None on unknown

const char* lease_action_name(LeaseAction action) noexcept;

// False for None and LanRemote. Those do not take the microphone or the speaker.
bool occupies_audio_lease(SessionKind kind) noexcept;

// holder is the session that currently owns the audio lease (None if idle).
// PhoneCall occupies that lease until the host releases it. It does not open
// our capture or playback device.
LeaseDecision preempt(SessionKind holder, SessionKind incoming) noexcept;

// Process-wide owner. preempt() is the policy; these functions apply it.
// A capture or playback backend must refuse to start without a live token.
// Acquiring a lease replaces the holder immediately and bumps the token. The
// previous holder observes lease_token() != its grant and stops. That is the
// handoff: there is no second acknowledgement channel. AAudio capture stops
// its callback when the token changes. Desktop capture is owned by the host,
// which must stop the device on the same check.
// An idle phone call takes the lease so a later session cannot open the mic.
// LanRemote still does not.
struct LeaseGrant {
    bool ok = false;
    LeaseAction action = LeaseAction::Deny;
    const char* reason = "";
    std::uint64_t token = 0;
    SessionKind holder = SessionKind::None;
};

LeaseGrant lease_acquire(SessionKind incoming);
// False when the token is not the current grant. A successful release bumps the
// token, so the same value cannot release a second time.
bool lease_release(std::uint64_t token);
void lease_force_clear() noexcept;
SessionKind lease_holder() noexcept;
std::uint64_t lease_token() noexcept;

// Capture and playback are different uses of the same lease.
bool lease_allows_capture(SessionKind kind) noexcept;
bool lease_allows_playback(SessionKind kind) noexcept;

} // namespace pm::audio::session
