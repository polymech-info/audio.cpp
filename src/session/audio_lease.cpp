#include "pm/audio/session/audio_lease.hpp"

#include <mutex>

namespace pm::audio::session {
namespace {

std::mutex& lease_mu()
{
    static std::mutex mu;
    return mu;
}

SessionKind g_holder = SessionKind::None;
std::uint64_t g_token = 0;

} // namespace

const char* session_kind_name(SessionKind kind) noexcept
{
    switch (kind) {
    case SessionKind::None:            return "none";
    case SessionKind::Dictation:       return "dictation";
    case SessionKind::ReadAloud:       return "readaloud";
    case SessionKind::AiConversation:  return "ai";
    case SessionKind::WalkieListen:    return "walkie_listen";
    case SessionKind::WalkieTransmit:  return "walkie_transmit";
    case SessionKind::GroupVoice:      return "group";
    case SessionKind::LanRemote:       return "lan_remote";
    case SessionKind::PhoneCall:       return "phone_call";
    case SessionKind::Recorder:        return "recorder";
    }
    return "none";
}

SessionKind parse_session_kind(std::string_view token) noexcept
{
    if (token == "none")             return SessionKind::None;
    if (token == "dictation")        return SessionKind::Dictation;
    if (token == "readaloud")        return SessionKind::ReadAloud;
    if (token == "ai")               return SessionKind::AiConversation;
    if (token == "walkie_listen")    return SessionKind::WalkieListen;
    if (token == "walkie_transmit")  return SessionKind::WalkieTransmit;
    if (token == "group")            return SessionKind::GroupVoice;
    if (token == "lan_remote")       return SessionKind::LanRemote;
    if (token == "phone_call")       return SessionKind::PhoneCall;
    if (token == "recorder")         return SessionKind::Recorder;
    return SessionKind::None;
}

const char* lease_action_name(LeaseAction action) noexcept
{
    switch (action) {
    case LeaseAction::Allow:   return "allow";
    case LeaseAction::Deny:    return "deny";
    case LeaseAction::Preempt: return "preempt";
    }
    return "deny";
}

bool occupies_audio_lease(SessionKind kind) noexcept
{
    return kind != SessionKind::None && kind != SessionKind::LanRemote;
}

LeaseDecision preempt(SessionKind holder, SessionKind incoming) noexcept
{
    if (incoming == SessionKind::None)
        return {LeaseAction::Deny, "no session"};

    if (holder == SessionKind::PhoneCall)
        return {LeaseAction::Deny, "call in progress"};

    if (incoming == SessionKind::PhoneCall) {
        if (holder == SessionKind::None)
            return {LeaseAction::Allow, "idle"};
        return {LeaseAction::Preempt, "call"};
    }

    if (incoming == holder)
        return {LeaseAction::Deny, "already active"};

    // LAN remote control never opens the phone mic or speaker.
    if (incoming == SessionKind::LanRemote)
        return {LeaseAction::Allow, "no device"};

    if (holder == SessionKind::None)
        return {LeaseAction::Allow, "idle"};

    if (holder == SessionKind::LanRemote)
        return {LeaseAction::Allow, "lan holds no device"};

    // Same channel: listening and transmitting are one membership, not two sessions.
    if (holder == SessionKind::WalkieListen && incoming == SessionKind::WalkieTransmit)
        return {LeaseAction::Allow, "transmit upgrade"};
    if (holder == SessionKind::WalkieTransmit && incoming == SessionKind::WalkieListen)
        return {LeaseAction::Allow, "return to listening"};

    // Starting an AI conversation replaces dictation. The reverse does not.
    if (holder == SessionKind::Dictation && incoming == SessionKind::AiConversation)
        return {LeaseAction::Preempt, "ai replaces dictation"};

    // A recording takes the microphone from dictation. A live session keeps it.
    if (holder == SessionKind::Dictation && incoming == SessionKind::Recorder)
        return {LeaseAction::Preempt, "recording replaces dictation"};

    // Read-aloud holds only the speaker and yields to any live session.
    if (holder == SessionKind::ReadAloud)
        return {LeaseAction::Preempt, "replaces read-aloud"};

    // A human group does not become an AI session on its own.
    if (holder == SessionKind::GroupVoice && incoming == SessionKind::AiConversation)
        return {LeaseAction::Deny, "group audio does not enter the voice agent"};

    return {LeaseAction::Deny, "audio lease busy"};
}

LeaseGrant lease_acquire(SessionKind incoming)
{
    std::lock_guard<std::mutex> lock(lease_mu());
    const LeaseDecision decision = preempt(g_holder, incoming);
    LeaseGrant grant;
    grant.action = decision.action;
    grant.reason = decision.reason;
    grant.holder = g_holder;
    if (decision.action == LeaseAction::Deny)
        return grant;
    // LAN remote never opens the microphone. A phone call does, including when
    // the lease was idle: the holder becomes PhoneCall until the host releases it.
    const bool takes_lease = incoming != SessionKind::LanRemote;
    if (!takes_lease) {
        grant.ok = true;
        grant.token = 0;
        return grant;
    }
    g_holder = incoming;
    g_token += 1;
    grant.ok = true;
    grant.token = g_token;
    grant.holder = g_holder;
    return grant;
}

bool lease_release(std::uint64_t token)
{
    std::lock_guard<std::mutex> lock(lease_mu());
    if (token == 0 || token != g_token)
        return false;
    g_holder = SessionKind::None;
    g_token += 1;
    return true;
}

void lease_force_clear() noexcept
{
    std::lock_guard<std::mutex> lock(lease_mu());
    g_holder = SessionKind::None;
    g_token += 1;
}

SessionKind lease_holder() noexcept
{
    std::lock_guard<std::mutex> lock(lease_mu());
    return g_holder;
}

std::uint64_t lease_token() noexcept
{
    std::lock_guard<std::mutex> lock(lease_mu());
    return g_token;
}

bool lease_allows_capture(SessionKind kind) noexcept
{
    return kind == SessionKind::Dictation
        || kind == SessionKind::AiConversation
        || kind == SessionKind::WalkieTransmit
        || kind == SessionKind::GroupVoice
        || kind == SessionKind::Recorder;
}

bool lease_allows_playback(SessionKind kind) noexcept
{
    return kind == SessionKind::ReadAloud
        || kind == SessionKind::AiConversation
        || kind == SessionKind::WalkieListen
        || kind == SessionKind::WalkieTransmit
        || kind == SessionKind::GroupVoice;
}

} // namespace pm::audio::session
