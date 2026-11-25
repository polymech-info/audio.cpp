#pragma once

// Phone recorder. One worker reads the AAudio ring and writes a WAV.
// Noise remover, echo removal, and volume leveling save at 16 kHz.
// With those off, the file is 48 kHz. The capture callback only copies.

#include <cstddef>
#include <cstdint>
#include <string>

namespace pm::audio::session {

struct RecorderRequest {
    std::uint64_t token = 0;
    std::string wav_path;
    std::string model_path;
    bool gtcrn = false;
    bool aec = false;
    bool agc = false;
};

struct RecorderStatus {
    bool ok = false;
    bool running = false;
    std::string reason;
    int opened_rate = 0;
    int file_rate = 16000;
    bool gtcrn_requested = false;
    bool gtcrn_active = false;
    bool aec_requested = false;
    bool agc_requested = false;
    bool aec_active = false;
    int session_id = -1; // AAudio session for platform AcousticEchoCanceler
    std::string filter_note;
    std::int64_t frames_in = 0;
    std::int64_t frames_written = 0;
    std::int64_t dropped = 0;
    std::int64_t process_ns = 0;
    std::int64_t last_process_ns = 0;
    int rms = 0;
    int peak = 0;
};

RecorderStatus recorder_start(const RecorderRequest& request);
RecorderStatus recorder_set(bool gtcrn, bool aec, bool agc);
// Keeps the mic open and drops samples, so the file does not grow while paused.
void recorder_pause(bool on);
RecorderStatus recorder_stop();
RecorderStatus recorder_stats();

// Copies the take to 16 kHz mono for a cloud live transcript. The file rate
// stays as started. Off drops the queue. The worker is still the only ring reader.
void recorder_tap(bool on);
// Drains queued 16 kHz frames. 0 when the tap is off or empty.
std::size_t recorder_pull_pcm(std::int16_t* dst, std::size_t max_samples);

// A second copy of the file-rate take, for a caller that plays it elsewhere.
// The transcript tap is left alone. Off drops this queue.
void recorder_play(bool on);
std::size_t recorder_play_pull(std::int16_t* dst, std::size_t max_samples);

// Resampler, WAV finalization, tap ring, and idle start/stop. Empty when every check passed.
std::string recorder_logic_checks();

} // namespace pm::audio::session
