#pragma once

// One playback session on a chosen speaker. Callers own the transport. The LLM
// server only mounts this on /v1/audio/outputs and /v1/audio/play*. It is not a
// mesh audio grant: audio=listen stays capture. Pushed audio is converted to
// the same 48 kHz stereo s16le contract as /v1/audio/stream.

#include <nlohmann/json.hpp>

#include <cstdint>
#include <memory>

namespace pm::audio {

struct SinkReply {
    int            status = 200;
    nlohmann::json body   = nlohmann::json::object();
};

class SinkHost {
public:
    SinkHost();
    ~SinkHost();

    SinkHost(const SinkHost&)            = delete;
    SinkHost& operator=(const SinkHost&) = delete;

    void stop();

    SinkReply status() const;
    SinkReply outputs();
    /// Body field device, name, or output. Empty picks the default speaker.
    /// A running session switches to that speaker.
    SinkReply select(const nlohmann::json& body);
    SinkReply start(const nlohmann::json& body);
    /// Interleaved s16. rate and channels describe pcm. 409 when nothing is open.
    SinkReply push(const std::int16_t* pcm, int frames, std::uint32_t rate, std::uint32_t channels);

private:
    struct Impl;
    std::shared_ptr<Impl> impl_;
};

} // namespace pm::audio
