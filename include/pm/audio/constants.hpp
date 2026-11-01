#pragma once

// Portable audio knobs, including the ElevenLabs voice and model defaults.
// Proxy and OpenAI model ids stay in the host (src/audio/audio_constants.hpp).

#include <cstddef>
#include <cstdint>

namespace pm::audio::constants {

inline constexpr std::size_t k_ring_cap = 512 * 1024;
inline constexpr std::size_t k_ring_prebuf = 16 * 1024;
inline constexpr int k_ring_push_wait_ms = 20;

inline constexpr int k_drain_ms = 40;
inline constexpr int k_stop_poll_ms = 100;

inline constexpr long k_curl_connect_timeout_s = 10;
inline constexpr long k_curl_transfer_timeout_s = 60;
inline constexpr std::size_t k_elevenlabs_reserve_bytes = 1024 * 1024;

inline constexpr bool k_apm_echo_cancel = true;
inline constexpr bool k_apm_hpf = true;
inline constexpr bool k_apm_ns = true;
inline constexpr bool k_apm_agc = true;
inline constexpr bool k_realtime_gtcrn = true;

inline constexpr std::uint32_t k_capture_sample_rate = 16000;
inline constexpr std::uint32_t k_capture_channels = 1;
inline constexpr std::uint32_t k_recording_sample_rate = 48000;
inline constexpr std::uint32_t k_recording_channels = 2;
inline constexpr std::uint32_t k_record_spill_threshold_seconds = 30;
inline constexpr std::uint32_t k_realtime_sample_rate = 24000;

inline constexpr int k_tts_ring_encoding_mp3 = 3;

inline constexpr const char* k_elevenlabs_output_format = "mp3_44100_128";
inline constexpr const char* k_elevenlabs_model_id = "eleven_turbo_v2_5";
inline constexpr const char* k_elevenlabs_voice_id = "tLK6fPv15M0oKv4V3ACR";
inline constexpr const char* k_proxy_default_voice = "EXAVITQu4vr4xnSDxMaL";
inline constexpr int k_elevenlabs_optimize_streaming_latency = 3;

inline constexpr bool k_elevenlabs_send_voice_settings = true;
inline constexpr double k_elevenlabs_stability = 0.9;
inline constexpr double k_elevenlabs_similarity_boost = 0.75;
inline constexpr double k_elevenlabs_style = 0.0;
inline constexpr bool k_elevenlabs_chunk_sentences = true;
inline constexpr std::size_t k_elevenlabs_chunk_target_chars = 400;
inline constexpr std::size_t k_elevenlabs_chunk_max_chars = 800;
inline constexpr std::size_t k_elevenlabs_max_prev_request_ids = 3;

inline constexpr int k_voiceover_sample_rate = 48000;
inline constexpr int k_voiceover_min_inter_gap_ms = 80;
inline constexpr double k_voiceover_max_stretch = 0.12;
inline constexpr int k_voiceover_max_utterance_ms = 8000;
inline constexpr int k_voiceover_merge_score_limit = 400;
inline constexpr int k_voiceover_last_pad_ms = 15000;
inline constexpr int k_voiceover_silence_thresh = 512;
inline constexpr int k_voiceover_trim_pad_ms = 20;
inline constexpr double k_voiceover_stability = 0.45;
inline constexpr double k_voiceover_similarity_boost = 0.80;
inline constexpr double k_voiceover_style = 0.20;
inline constexpr bool k_voiceover_speaker_boost = true;

inline constexpr std::size_t k_tts_text_file_max_bytes = 512 * 1024;
inline constexpr std::size_t k_tts_text_max_chars = 32768;

} // namespace pm::audio::constants
