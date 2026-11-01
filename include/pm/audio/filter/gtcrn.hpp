#pragma once

#include "pm/audio/filter/audio_filter.hpp"

#include <atomic>
#include <cstdint>
#include <functional>
#include <memory>
#include <vector>

namespace pm::audio::filter::gtcrn {

bool available();
std::string backend_name();

Result apply_file(const std::filesystem::path& src,
                  const std::filesystem::path& dst,
                  const Options& opt);

class StreamSession {
public:
    StreamSession();
    ~StreamSession();
    StreamSession(const StreamSession&) = delete;
    StreamSession& operator=(const StreamSession&) = delete;

    bool open(const std::string& model_hint, std::string& err);
    void close();
    bool is_open() const noexcept;

    void process(const int16_t* in, size_t frames, std::vector<int16_t>& out,
                 const std::atomic_bool* cancel = nullptr);

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

class LiveHop {
public:
    LiveHop();
    ~LiveHop();
    LiveHop(const LiveHop&) = delete;
    LiveHop& operator=(const LiveHop&) = delete;

    bool start(std::function<void(const int16_t*, size_t)> out, std::string& err);
    void enqueue(const int16_t* pcm, size_t frames);
    void stop();
    bool running() const noexcept;

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

} // namespace pm::audio::filter::gtcrn
