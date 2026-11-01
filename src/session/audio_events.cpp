#include "pm/audio/session/audio_events.hpp"

#include <cstdio>

namespace pm::audio::session {
namespace {

void append_escaped(std::string& out, std::string_view s)
{
    out.push_back('"');
    for (unsigned char c : s) {
        switch (c) {
        case '"':  out += "\\\""; break;
        case '\\': out += "\\\\"; break;
        case '\n': out += "\\n"; break;
        case '\r': out += "\\r"; break;
        case '\t': out += "\\t"; break;
        default:
            if (c < 0x20) {
                char buf[8];
                std::snprintf(buf, sizeof(buf), "\\u%04x", c);
                out += buf;
            } else {
                out.push_back(static_cast<char>(c));
            }
            break;
        }
    }
    out.push_back('"');
}

bool looks_like_json(std::string_view s)
{
    if (s.empty())
        return false;
    const char c = s.front();
    if (c == '{' || c == '[' || c == '"')
        return true;
    if (s == "true" || s == "false" || s == "null")
        return true;
    std::size_t i = 0;
    if (s[i] == '-')
        ++i;
    return i < s.size() && s[i] >= '0' && s[i] <= '9';
}

} // namespace

const char* audio_method_name(AudioMethod method) noexcept
{
    switch (method) {
    case AudioMethod::Ready:            return "realtimeReady";
    case AudioMethod::TextDelta:        return "realtimeTextDelta";
    case AudioMethod::TranscriptDelta:  return "realtimeTranscriptDelta";
    case AudioMethod::UserTranscript:   return "realtimeUserTranscript";
    case AudioMethod::ResponseDone:     return "realtimeResponseDone";
    case AudioMethod::Error:            return "realtimeError";
    case AudioMethod::AuthError:        return "realtimeAuthError";
    case AudioMethod::NetworkError:     return "realtimeNetworkError";
    case AudioMethod::AppendText:       return "appendText";
    case AudioMethod::Done:             return "realtimeDone";
    case AudioMethod::AgentStarted:     return "agentStarted";
    case AudioMethod::AgentDone:        return "agentDone";
    case AudioMethod::ConsentRequest:   return "consentRequest";
    case AudioMethod::ConsentDismiss:   return "dismissConsent";
    case AudioMethod::AskUser:          return "askUser";
    case AudioMethod::Level:            return "realtimeLevel";
    case AudioMethod::DeviceLost:       return "deviceLost";
    case AudioMethod::WalkieState:      return "walkieState";
    case AudioMethod::FloorGrant:       return "floorGrant";
    case AudioMethod::FloorDeny:        return "floorDeny";
    case AudioMethod::FloorReleased:    return "floorReleased";
    }
    return "realtimeError";
}

std::string envelope_json(const AudioEnvelope& event)
{
    std::string out;
    out.reserve(128 + event.session_id.size() + event.args_json.size());
    out += "{\"session_id\":";
    append_escaped(out, event.session_id);
    out += ",\"session_kind\":";
    append_escaped(out, session_kind_name(event.kind));
    out += ",\"seq\":";
    out += std::to_string(event.seq);
    out += ",\"ts_ms\":";
    out += std::to_string(event.ts_ms);
    out += ",\"participant_id\":";
    append_escaped(out, event.participant_id);
    out += ",\"method\":";
    append_escaped(out, audio_method_name(event.method));
    out += ",\"args\":";
    if (looks_like_json(event.args_json))
        out.append(event.args_json);
    else
        out += "[]";
    out += "}";
    return out;
}

} // namespace pm::audio::session
