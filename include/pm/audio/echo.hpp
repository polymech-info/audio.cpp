#pragma once

#include "pm/audio/constants.hpp"

#include <cstddef>
#include <cstdint>
#include <memory>
#include <string>

namespace pm::audio {

struct ApmBackend {
    void* (*create)(int sample_rate_hz, int channels, bool agc, bool ns) = nullptr;
    void (*destroy)(void* handle) = nullptr;
    void (*set_delay_ms)(void* handle, int ms) = nullptr;
    void (*process_render)(void* handle, const std::int16_t* pcm, std::size_t frames) = nullptr;
    void (*process_capture)(void* handle, std::int16_t* pcm, std::size_t frames) = nullptr;
    bool (*active)(void* handle) = nullptr;
};

// Installed by the host. With no backend, create() returns null and capture is unchanged.
void set_apm_backend(const ApmBackend& backend);

class EchoCancellationProcessor {
public:
    struct Config {
        int  sample_rate_hz = 16000;
        int  channels       = 1;
        bool agc            = constants::k_apm_agc;
        bool ns             = constants::k_apm_ns;
    };

    static std::unique_ptr<EchoCancellationProcessor> create(Config cfg);

    ~EchoCancellationProcessor();

    EchoCancellationProcessor(const EchoCancellationProcessor&) = delete;
    EchoCancellationProcessor& operator=(const EchoCancellationProcessor&) = delete;

    bool active() const;
    const std::string& backend_name() const;
    void set_stream_delay_ms(int ms);
    void process_render_reference(const int16_t* pcm, std::size_t frames);
    void process_capture(int16_t* pcm, std::size_t frames);

private:
    explicit EchoCancellationProcessor(Config cfg);

    Config cfg_;
    void* handle_ = nullptr;
    bool active_ = false;
    std::string backend_ = "none";
};

} // namespace pm::audio
