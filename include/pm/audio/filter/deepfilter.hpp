#pragma once

#include "pm/audio/filter/audio_filter.hpp"

#include <cstdint>
#include <memory>
#include <vector>

namespace pm::audio::filter::dfn {

bool available();
std::string backend_name();
std::filesystem::path discover_model(const std::string& explicit_path);
Result enhance_48k_mono(std::vector<float>& pcm,
                        const Options& opt,
                        const std::filesystem::path& src_for_temp);

class StreamSession {
public:
    StreamSession();
    ~StreamSession();
    StreamSession(const StreamSession&) = delete;
    StreamSession& operator=(const StreamSession&) = delete;

    bool open(const Options& opt, std::string& err);
    void close();
    bool is_open() const noexcept;

    void process(const int16_t* in, size_t frames, uint16_t channels,
                 std::vector<int16_t>& out);
    void flush(std::vector<int16_t>& out);

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

} // namespace pm::audio::filter::dfn
