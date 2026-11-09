#pragma once

// Direct AAudio capture and playback for Android.
// miniaudio's AAudio backend reports no real device names, reopens the default
// device after a disconnect, and does not expose the stream's device id.
// This wrapper opens one stream, checks the id that was actually opened, and
// treats a disconnect as device loss. It never opens a stream without a lease token.

#include "pm/audio/session/audio_lease.hpp"

#include <cstdint>
#include <string>

namespace pm::audio::session {

struct AAudioStatus {
    bool ok = false;
    bool running = false;
    bool device_lost = false;
    bool available = false;
    int opened_id = -1;
    int sample_rate = 0;
    std::int64_t frames = 0;
    std::int64_t dropped = 0;
    std::string reason;
};

// False when libaaudio.so cannot be loaded (Android 7 and older).
bool aaudio_available();

// specific=false leaves the device unspecified. specific=true must open that id
// or fail without leaving a stream running. preset is "recognition" or "communication".
// sample_rate is a request; the opened stream rate may differ. 0 uses 16 kHz.
AAudioStatus aaudio_capture_start(std::uint64_t token, int device_id, bool specific, const char* preset,
                                   int sample_rate = 16000);

AAudioStatus aaudio_capture_stop();
// Stop capture and write the kept PCM as a 16-bit mono WAV. Empty path only stops.
AAudioStatus aaudio_capture_stop_wav(const std::string& path);
AAudioStatus aaudio_capture_status();
// Drain captured mono s16 while the stream is running. 0 when nothing is queued.
// `rate` receives the opened stream rate. The capture callback only writes the ring.
std::size_t aaudio_capture_read(std::int16_t* dst, std::size_t max_samples, int* rate);

// Blocks until the tone finishes, the stream fails, or the duration elapses.
AAudioStatus aaudio_play_tone(std::uint64_t token, int device_id, bool specific, int duration_ms);

} // namespace pm::audio::session
