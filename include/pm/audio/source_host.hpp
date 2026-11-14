#pragma once

// One capture session: a microphone, a loopback endpoint, or both when mix is built.
// Callers own the transport. The LLM server mounts this on /v1/audio/*.

#include <nlohmann/json.hpp>

#include <cstdint>
#include <memory>

namespace pm::audio {

struct SourceReply {
    int            status = 200;
    nlohmann::json body   = nlohmann::json::object();
};

class SourceHost {
public:
    SourceHost();
    ~SourceHost();

    SourceHost(const SourceHost&)            = delete;
    SourceHost& operator=(const SourceHost&) = delete;

    void stop();

    SourceReply status() const;
    SourceReply sources();
    /// Interleaved s16 from the running capture. Returns frames, 0 on timeout,
    /// or -1 when nothing is running. One reader; a slow reader drops old samples.
    int pull(std::int16_t* dst, int max_frames, int wait_ms);
    /// Body fields: input, and for mix also mic and loopback.
    SourceReply start(const nlohmann::json& body);
    /// Switch a running session, or store input while idle.
    SourceReply select(const nlohmann::json& body);

private:
    struct Impl;
    std::shared_ptr<Impl> impl_;
};

} // namespace pm::audio
