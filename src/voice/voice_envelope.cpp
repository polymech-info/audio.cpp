#include "pm/audio/voice/voice_envelope.hpp"

namespace pm::audio::voice {

nlohmann::json trigger_envelope_json(const TriggerEvent& event)
{
    return nlohmann::json{
        {"type", k_trigger_envelope_type},
        {"id", event.id},
        {"timestamp_ms", event.timestamp_ms},
        {"source", event.source},
        {"route", event.route},
        {"text", event.text},
        {"raw_text", event.raw_text},
        {"wake_prefix", event.wake_prefix},
        {"confidence", event.confidence},
        {"language", event.language},
        {"enhancement", {
            {"requested", event.requested_filter},
            {"effective", event.effective_filter},
            {"fallback", event.filter_fallback},
        }},
    };
}

nlohmann::json cancel_envelope_json(const CancelEvent& event)
{
    return nlohmann::json{
        {"type", k_cancel_envelope_type},
        {"id", event.id},
        {"timestamp_ms", event.timestamp_ms},
        {"source", event.source},
        {"reason", event.reason},
    };
}

std::string serialize_trigger_envelope(const TriggerEvent& event)
{
    return trigger_envelope_json(event).dump();
}

std::string serialize_cancel_envelope(const CancelEvent& event)
{
    return cancel_envelope_json(event).dump();
}

nlohmann::json transcript_envelope_json(const Transcript& transcript,
                                        const MatchResult& match)
{
    return nlohmann::json{
        {"type", k_transcript_envelope_type},
        {"schema", k_transcript_envelope_type},
        {"text", transcript.text},
        {"normalized", match.normalized_text},
        {"command", match.command_text},
        {"wake_prefix", match.wake_prefix},
        {"matched", match.matched},
        {"control", control_intent_name(match.control)},
        {"confidence", transcript.confidence},
        {"language", transcript.language},
    };
}

} // namespace pm::audio::voice
