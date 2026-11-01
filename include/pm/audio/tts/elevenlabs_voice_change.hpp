#pragma once

#include <cstdint>
#include <string>
#include <vector>

namespace pm::tts {

struct ElevenLabsVoiceChangeConfig {
    std::string api_key;
    std::string voice_id;
    std::string base_url = "https://api.elevenlabs.io";
    std::string model_id = "eleven_multilingual_sts_v2";
    std::string output_format = "mp3_44100_128";
    std::string input_format = "other";
    std::string voice_settings_json;
    int         optimize_streaming_latency = 0;
    int64_t     seed = -1;
    long        transfer_timeout_seconds = 300;
    bool        remove_background_noise = false;
    bool        enable_logging = true;
};

/// Convert the voice in @p source_audio_path with ElevenLabs speech-to-speech.
/// Returns the encoded response body in the requested output format.
/// Throws std::invalid_argument for invalid configuration and
/// std::runtime_error for file, transport, or API errors.
std::vector<uint8_t> elevenlabs_voice_change(
    const std::string& source_audio_path,
    const ElevenLabsVoiceChangeConfig& config);

/// Select an encoded speech-to-speech format from a lowercase destination
/// extension. WAV uses a WAV container (not the raw `pcm_*` response).
std::string elevenlabs_voice_change_format_for_ext(const std::string& ext_lower);

} // namespace pm::tts
