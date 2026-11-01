#pragma once

#include "pm/audio/voice/voice_types.hpp"

#include <string>
#include <string_view>

namespace pm::audio::voice {

/// Deterministic command normalization: Unicode punctuation and Whisper
/// artifacts (`[…]`, `(…)`, spoken "comma"/"period") become spaces, ASCII
/// case-folds, fillers (`um`, leading `okay`/`hey`) drop, whitespace collapses.
std::string normalize_voice_text(std::string_view text);

/// Pure wake-prefix and control-intent matching; performs no I/O or mutation.
MatchResult match_voice_text(std::string_view text, const MatchConfig& config);

/// Empty / whitespace wake disables the prefix: the whole utterance is the
/// command (`wake_required = false`). Non-empty keeps the current required prefix.
void apply_wake_phrase(MatchConfig& config, std::string_view wake);

} // namespace pm::audio::voice
