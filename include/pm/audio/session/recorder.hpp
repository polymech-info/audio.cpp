#pragma once

// Phone recorder. One worker reads the AAudio ring, resamples to 16 kHz,
// optionally runs GTCRN, and writes a WAV. The capture callback only copies.

#include <array>
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
    std::string filter_note;
    std::int64_t frames_in = 0;
    std::int64_t frames_written = 0;
    std::int64_t dropped = 0;
    std::int64_t process_ns = 0;
    std::int64_t last_process_ns = 0;
    int rms = 0;
    int peak = 0;
    std::array<int, 16> bands{};
};

RecorderStatus recorder_start(const RecorderRequest& request);
RecorderStatus recorder_set(bool gtcrn, bool aec, bool agc);
RecorderStatus recorder_stop();
RecorderStatus recorder_stats();

} // namespace pm::audio::session
