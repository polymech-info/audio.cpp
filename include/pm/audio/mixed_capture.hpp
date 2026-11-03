#pragma once
//
// mixed_audio_capture.hpp — mic / desktop / camera / mix capture for recording.

#ifndef PM_HAS_AUDIO_IO
#define PM_HAS_AUDIO_IO 1
#endif

#if PM_HAS_AUDIO_IO

#include "pm/audio/capture.hpp"

#include <cstddef>
#include <cstdint>
#include <functional>
#include <memory>
#include <string>

namespace pm::audio {

using CameraNameFn = std::string (*)(const std::string&);
using CancelSubscribeFn = void (*)(void (*)());

void set_camera_name_for_capture(CameraNameFn fn);
void set_mixed_capture_cancel_subscribe(CancelSubscribeFn fn);

/// Resolve a WASAPI capture device name for @p camera_input (video substring / default).
/// Empty result → use system default capture (AudioInput empty-name behavior).
std::string resolve_camera_audio_device(const std::string& camera_input);

/// Enable camera mic on @p cfg from creator-fx fields (resolves device from @p camera_input).
void apply_camera_audio(MixedCaptureConfig& cfg,
                        bool enable,
                        const std::string& camera_input,
                        float gain) noexcept;

/// Build config from CLI/xblox string fields (@p source_token may be empty → @p default_source).
MixedCaptureConfig make_mixed_capture_config(const std::string& source_token,
                                             const std::string& mic_device,
                                             const std::string& desktop_device,
                                             float mic_gain,
                                             float desktop_gain,
                                             CaptureSource default_source = CaptureSource::none);

/// Same as make_mixed_capture_config but pins 48 kHz stereo (CLI / xblox file recording).
MixedCaptureConfig make_recording_capture_config(const std::string& source_token,
                                                 const std::string& mic_device,
                                                 const std::string& desktop_device,
                                                 float mic_gain,
                                                 float desktop_gain,
                                                 CaptureSource default_source = CaptureSource::mic);

class MixedAudioCapture {
public:
    MixedAudioCapture();
    ~MixedAudioCapture();

    MixedAudioCapture(const MixedAudioCapture&)            = delete;
    MixedAudioCapture& operator=(const MixedAudioCapture&) = delete;

    /// Live captures register on the CLI cancel sink; Ctrl+C / HUD Stop call stop().
    /// Start streaming mixed PCM to @p on_pcm. Returns false + @p err on device failure.
    /// @p cfg.source == none and !camera_audio is a no-op success (is_running() stays false).
    /// Callback @p frame_count is per-channel frames (miniaudio convention).
    bool start(const MixedCaptureConfig& cfg,
               std::function<void(const int16_t*, size_t frame_count)> on_pcm,
               std::string& err);

    void stop();
    bool is_running() const noexcept;

    const std::string& opened_mic_device() const noexcept;
    const std::string& opened_desktop_device() const noexcept;
    const std::string& opened_camera_device() const noexcept;

    uint32_t pcm_sample_rate() const noexcept;
    uint32_t pcm_channels() const noexcept;

    /// Optional speech-mic tap (mic / camera legs only — not desktop loopback).
    /// Called from the capture callback; keep it cheap.
    void set_speech_meter(std::function<void(const int16_t*, size_t, uint32_t, uint32_t)> fn);

    /// Mic-path reroute signal (WASAPI device unplugged). False for desktop/camera-only.
    bool was_mic_rerouted() const;

private:
    struct Impl;
    std::unique_ptr<Impl> m_;
};

} // namespace pm::audio

#endif // PM_HAS_AUDIO_IO
