#pragma once

// One capture session: a microphone, a loopback endpoint, or a mix of several
// of each. A mix list carries a gain per source; the host sums them into one
// stream. Callers own the transport. The LLM server mounts this on /v1/audio/*.

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

    /// Used when a start body omits sampleRate or bits. 16000, 24000, or 48000, and 16.
    static void set_capture_format(std::uint32_t sample_rate, std::uint32_t bits);

    SourceHost(const SourceHost&)            = delete;
    SourceHost& operator=(const SourceHost&) = delete;

    /// Stops whatever is running. Process shutdown uses this.
    void stop();
    /// Stops only when `session` is 0 or equals the running capture.
    /// A stale id leaves the newer capture running and returns false.
    bool stop_session(std::uint64_t session);

    SourceReply status() const;
    SourceReply sources();
    /// One shared cursor. A second caller of pull splits samples. Streams use
    /// subscribe so each reader keeps its own cursor.
    int pull(std::int16_t* dst, int max_frames, int wait_ms);
    int pull_session(std::uint64_t session, std::int16_t* dst, int max_frames, int wait_ms);
    /// A cursor starts at the live edge. 0 means the session is not running.
    /// A slow cursor drops its own oldest samples and does not move the others.
    std::uint64_t subscribe(std::uint64_t session);
    void unsubscribe(std::uint64_t session, std::uint64_t cursor);
    int pull_cursor(std::uint64_t session, std::uint64_t cursor, std::int16_t* dst, int max_frames, int wait_ms);
    /// Body fields: input, sampleRate (16000, 24000, or 48000), bits (16),
    /// and for mix also mic and loopback, or sources[{spec,gain}].
    SourceReply start(const nlohmann::json& body);
    /// Switch a running session, or store input while idle.
    SourceReply select(const nlohmann::json& body);

private:
    struct Impl;
    std::shared_ptr<Impl> impl_;
};

} // namespace pm::audio
