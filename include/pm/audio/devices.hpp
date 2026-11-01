#pragma once

#include "pm/audio/constants.hpp"
#include "pm/audio/ring.hpp"

#include <atomic>
#include <condition_variable>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <memory>
#include <mutex>
#include <string>
#include <vector>

namespace pm::audio {

// Called from AudioInput's constructor so the host can install its log sink
// and the echo-cancellation backend before the device opens.
using DevicePrepareFn = void (*)();
void set_device_prepare(DevicePrepareFn fn);

struct DeviceInfo {
    std::string name;
    bool        is_default  = false;
    bool        is_loopback = false;
    uint32_t    channels    = 0;
    uint32_t    sample_rate = 0;
};

std::vector<DeviceInfo> enumerate_capture_devices();
std::vector<DeviceInfo> enumerate_playback_devices();
std::vector<DeviceInfo> enumerate_loopback_devices();

class AudioInput {
public:
    AudioInput();
    ~AudioInput();

    AudioInput(const AudioInput&)            = delete;
    AudioInput& operator=(const AudioInput&) = delete;

    void start(std::function<void(const int16_t* pcm, size_t frame_count)> on_pcm,
               const std::string& device_name = "",
               uint32_t sample_rate = constants::k_capture_sample_rate,
               uint32_t channels    = constants::k_capture_channels,
               bool apm_ns          = constants::k_apm_ns);

    void stop();
    void process_render_reference(const int16_t* pcm, size_t frame_count);
    void set_aec_stream_delay_ms(int ms);
    bool is_running() const;
    const std::string& opened_device_name() const;
    bool was_rerouted() const;

private:
    struct Impl;
    std::unique_ptr<Impl> m_;
};

class DesktopAudioCapture {
public:
    DesktopAudioCapture();
    ~DesktopAudioCapture();

    DesktopAudioCapture(const DesktopAudioCapture&)            = delete;
    DesktopAudioCapture& operator=(const DesktopAudioCapture&) = delete;

    bool start(std::function<void(const int16_t* pcm, size_t frame_count)> on_pcm,
               const std::string& device_name,
               std::string& err,
               uint32_t sample_rate = constants::k_capture_sample_rate,
               uint32_t channels    = constants::k_capture_channels);

    void stop();
    bool is_running() const;
    const std::string& opened_device_name() const;

private:
    struct Impl;
    std::unique_ptr<Impl> m_;
};

void play_sound_file_async(const std::string& file_path);
void play_sound_file_sync(const std::string& file_path,
                          std::function<bool()> cancel_fn = {});
void stop_sound_file();

class AudioOutput {
public:
    AudioOutput();
    ~AudioOutput();

    AudioOutput(const AudioOutput&)            = delete;
    AudioOutput& operator=(const AudioOutput&) = delete;

    void set_device_name(const std::string& name);
    void play_sync(const uint8_t* data, size_t bytes);
    void play_ring(AudioRingBuffer& ring, int encoding);
    void pause();
    void resume();
    void stop();
    bool is_playing() const;

    struct Impl;

private:
    std::unique_ptr<Impl> impl_;
};

bool capture_device_matches(const std::string& device_name);

} // namespace pm::audio
