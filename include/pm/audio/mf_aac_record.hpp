#pragma once

#include <cstdint>
#include <filesystem>
#include <memory>
#include <string>

namespace pm::audio {

#if defined(_WIN32)

/// Windows Media Foundation AAC-LC recorder (.m4a / .aac).
/// Accepts PCM during capture; MF encodes on WriteSample (must not block the callback long).
class MfAacRecordWriter {
public:
    MfAacRecordWriter();
    ~MfAacRecordWriter();

    MfAacRecordWriter(const MfAacRecordWriter&)            = delete;
    MfAacRecordWriter& operator=(const MfAacRecordWriter&) = delete;

    /// Open @p dst for AAC encoding. Resamples to 48 kHz when needed (MF requirement).
    bool open(const std::filesystem::path& dst,
              uint32_t sample_rate,
              uint16_t channels,
              std::string& err);

    /// Thread-safe. @p frame_count is per-channel frames.
    void write_pcm(const int16_t* pcm, size_t frame_count);

    /// Finalize the container. No-op when not open.
    bool close(std::string& err);

    bool     is_open() const noexcept;
    uint32_t frame_count() const noexcept;
    uint32_t sample_rate() const noexcept;
    uint16_t channels() const noexcept;

private:
    struct Impl;
    std::unique_ptr<Impl> m_;
};

#endif

} // namespace pm::audio
