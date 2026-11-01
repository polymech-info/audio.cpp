#pragma once

#include "pm/audio/ring.hpp"
#include "pm/audio/constants.hpp"

#include <cstdint>
#include <functional>
#include <string>
#include <vector>

namespace pm::tts {

namespace C = pm::audio::constants;

struct TtsSynthesisOptions {
    bool        send_voice_settings  = C::k_elevenlabs_send_voice_settings;
    double      stability            = C::k_elevenlabs_stability;
    double      similarity_boost     = C::k_elevenlabs_similarity_boost;
    double      style                = C::k_elevenlabs_style;
    bool        chunk_sentences      = C::k_elevenlabs_chunk_sentences;
    std::size_t chunk_target_chars   = C::k_elevenlabs_chunk_target_chars;
    std::size_t chunk_max_chars      = C::k_elevenlabs_chunk_max_chars;
    std::size_t max_prev_request_ids = C::k_elevenlabs_max_prev_request_ids;
    /// ElevenLabs TTS speed (0.7–1.2). 1.0 leaves delivery unchanged.
    double      speed                = 1.0;
    /// Best-effort consistency seed. < 0 omits the field.
    std::int64_t seed                = -1;
    bool         use_speaker_boost   = true;
    /// Empty omits the field. "auto" | "on" | "off".
    std::string  apply_text_normalization;
};

struct TtsSynthResult {
    std::vector<uint8_t> audio;
    std::string          request_id;
};

struct ElevenLabsTTSConfig {
    std::string api_key;
    std::string voice_id      = C::k_elevenlabs_voice_id;
    std::string model_id      = C::k_elevenlabs_model_id;
    std::string output_format = C::k_elevenlabs_output_format;

    bool        send_voice_settings  = C::k_elevenlabs_send_voice_settings;
    double      stability            = C::k_elevenlabs_stability;
    double      similarity_boost     = C::k_elevenlabs_similarity_boost;
    double      style                = C::k_elevenlabs_style;
    bool        chunk_sentences      = C::k_elevenlabs_chunk_sentences;
    std::size_t chunk_target_chars   = C::k_elevenlabs_chunk_target_chars;
    std::size_t chunk_max_chars      = C::k_elevenlabs_chunk_max_chars;
    std::size_t max_prev_request_ids = C::k_elevenlabs_max_prev_request_ids;

    using ShouldCancelFn = bool (*)();
    ShouldCancelFn should_cancel = nullptr;

    /// ElevenLabs TTS speed (0.7–1.2). 1.0 leaves delivery unchanged.
    double       speed = 1.0;
    std::int64_t seed  = -1;

    bool         use_speaker_boost = true;
    std::string  apply_text_normalization;

    /// Stitching: if request IDs are set, the matching *_text field is omitted
    /// (ElevenLabs ignores previous_text when previous_request_ids is present).
    std::string              context_previous_text;
    std::string              context_next_text;
    std::vector<std::string> context_previous_request_ids;
    std::vector<std::string> context_next_request_ids;
};

void apply_synthesis_options(ElevenLabsTTSConfig& cfg, const TtsSynthesisOptions& opts);

// Synchronous: fetches all audio bytes then returns them.
// Throws std::runtime_error on network or API error.
std::vector<uint8_t> elevenlabs_tts_synthesize(const std::string& text,
                                                const ElevenLabsTTSConfig& cfg);

/// Same as synthesize, plus the last `request-id` response header (stitching).
TtsSynthResult elevenlabs_tts_synthesize_result(const std::string& text,
                                                const ElevenLabsTTSConfig& cfg);

// Streaming variant: pushes audio chunks into `ring` as they arrive.
// Returns when the transfer completes or fails; marks ring done either way.
void elevenlabs_tts_synthesize_stream(const std::string&          text,
                                      const ElevenLabsTTSConfig&  cfg,
                                      pm::audio::AudioRingBuffer& ring);

// Pick a sensible ElevenLabs output_format from a file extension.
//   .wav  → "pcm_44100"    (raw s16le — write WAV header yourself)
//   .mp3  → "mp3_44100_128"
//   .opus → "opus_48000_32"
//   else  → "mp3_44100_128"
std::string tts_format_for_ext(const std::string& ext_lower);

// True for raw PCM that needs a WAV wrapper: ElevenLabs "pcm_44100" or OpenRouter "pcm".
bool tts_format_is_pcm(const std::string& fmt);

// Sample rate from "pcm_44100" → 44100. Bare "pcm" is OpenRouter/xAI 24 kHz s16le.
int tts_pcm_sample_rate(const std::string& fmt);

} // namespace pm::tts
