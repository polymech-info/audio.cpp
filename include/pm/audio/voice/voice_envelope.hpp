#pragma once

#include "pm/audio/voice/voice_types.hpp"

#include <nlohmann/json.hpp>

#include <string>

namespace pm::audio::voice {

inline constexpr const char k_trigger_envelope_type[] = "pm.voice.trigger/v1";
inline constexpr const char k_cancel_envelope_type[] = "pm.voice.cancel/v1";
inline constexpr const char k_transcript_envelope_type[] = "pm.voice.transcript/v1";

nlohmann::json trigger_envelope_json(const TriggerEvent& event);
nlohmann::json cancel_envelope_json(const CancelEvent& event);
nlohmann::json transcript_envelope_json(const Transcript& transcript,
                                        const MatchResult& match);

std::string serialize_trigger_envelope(const TriggerEvent& event);
std::string serialize_cancel_envelope(const CancelEvent& event);

} // namespace pm::audio::voice
