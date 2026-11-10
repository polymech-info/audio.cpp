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

    /// ISO 639-1, or ast / yue / ceb / fil / kam / ckb. Empty lets the model detect.
    /// Omitted for eleven_multilingual_v2, which rejects the field.
    std::string  language_code;
    /// 0–1. Sent only for eleven_v4 and eleven_v4_turbo. < 0 omits it.
    double       clone_strength = -1;
    /// Japanese text normalization. Off unless set. It is slow.
    bool         apply_language_text_normalization = false;
    /// Use the instant-clone read of a professional voice.
    bool         use_pvc_as_ivc = false;

    struct PronunciationLocator {
        std::string id;
        std::string version_id;
    };
    std::vector<PronunciationLocator> pronunciation_dictionaries;

    /// Unset sentinels: distance and send_level < 0, pan > 1.
    struct AudioEffects {
        std::string filter_preset_id;
        double      distance = -1;
        std::string environment_id;
        std::string background_noise_id;
        double      send_level = -1;
        double      pan = 2;
    };
    AudioEffects audio_effects;

    /// Stitching: if request IDs are set, the matching *_text field is omitted
    /// (ElevenLabs ignores previous_text when previous_request_ids is present).
    std::string              context_previous_text;
    std::string              context_next_text;
    std::vector<std::string> context_previous_request_ids;
    std::vector<std::string> context_next_request_ids;
};

void apply_synthesis_options(ElevenLabsTTSConfig& cfg, const TtsSynthesisOptions& opts);

// Sentence chunks for one TTS job. A single element when chunking is off
// or the text already fits in chunk_max_chars.
std::vector<std::string> tts_text_chunks(const std::string& text,
                                         const ElevenLabsTTSConfig& cfg);

// True for a 2-letter code or ast, yue, ceb, fil, kam, ckb.
bool tts_language_code_ok(const std::string& code);

// pcm / ulaw / alaw reject reverb and a non-zero pan. Empty when the pair is legal.
std::string tts_effects_format_error(const ElevenLabsTTSConfig& cfg);

// Query is output_format only. enable_logging stays at the API default (on):
// stitching is unavailable when it is off. optimize_streaming_latency is not
// sent. It is deprecated, and level 4 turns the text normalizer off.
std::string elevenlabs_tts_request_url(const std::string& voice_id,
                                       const std::string& output_format,
                                       bool stream);

// One convert body. v4 sends stability and similarity only, plus clone_strength
// when set. Style, speed, and SSML are left out for v4.
std::string tts_request_body(const ElevenLabsTTSConfig& cfg, const std::string& text);

// Fields merged onto an OpenAI /audio/speech body. Includes output_format when
// cfg.output_format is the ElevenLabs codec_rate_bitrate form.
std::string tts_proxy_fields_json(const ElevenLabsTTSConfig& cfg,
                                  const std::string& previous_text,
                                  const std::string& next_text);

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
