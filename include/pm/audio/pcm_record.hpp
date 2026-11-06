#pragma once

#include <condition_variable>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <limits>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

namespace pm::audio {

struct PcmLevelStats {
    float    peak_dbfs         = -100.0f;
    float    rms_dbfs          = -100.0f;
    uint64_t clipped_samples   = 0;
    uint64_t total_samples     = 0;
};

enum class RecordContainer {
    Wav,
    Aac,
};

/// @p ext_lower must already be lower-case (e.g. ".wav", ".m4a").
RecordContainer record_container_from_extension(const std::string& ext_lower);

/// True when AAC/M4A recording is available on this platform.
bool aac_record_supported() noexcept;

/// Merge @p sample_count interleaved s16 values into running @p stats.
void accumulate_pcm_levels(PcmLevelStats& stats, const int16_t* samples, size_t sample_count);

/// Scan interleaved s16 PCM (@p sample_count int16 values, not frames).
PcmLevelStats analyze_pcm_levels(const int16_t* samples, size_t sample_count);

/// Samples that fit in a canonical 44-byte RIFF PCM header.
/// A longer capture must stay in the buffer; this function does not invent RF64.
inline bool pcm_wav_header_fits(uint64_t sample_count, uint32_t sample_rate, uint16_t channels) noexcept
{
    if (channels == 0 || sample_rate == 0 || sample_count == 0)
        return false;
    if (sample_count > (std::numeric_limits<uint64_t>::max() / 2))
        return false;
    const uint64_t data_bytes = sample_count * 2;
    if (data_bytes > 0xFFFFFFFFull - 36)
        return false;
    const uint64_t byte_rate = static_cast<uint64_t>(sample_rate) * channels * 2u;
    if (byte_rate > 0xFFFFFFFFull)
        return false;
    if (static_cast<uint32_t>(channels) * 2u > 0xFFFFu)
        return false;
    return true;
}

/// Spill threshold in samples. The product is capped instead of wrapping.
inline uint32_t spill_sample_threshold(uint32_t seconds, uint32_t sample_rate, uint16_t channels) noexcept
{
    if (seconds == 0 || sample_rate == 0 || channels == 0)
        return 0;
    if (seconds > std::numeric_limits<uint32_t>::max() / sample_rate)
        return std::numeric_limits<uint32_t>::max();
    const uint64_t frames = static_cast<uint64_t>(seconds) * sample_rate;
    if (frames > std::numeric_limits<uint32_t>::max() / channels)
        return std::numeric_limits<uint32_t>::max();
    return static_cast<uint32_t>(frames * channels);
}

/// Interleaved s16 PCM. @c push frame counts are per channel.
/// One capture thread calls @c push. A worker owns RAM growth and the spill file.
/// @c push copies into a fixed ring and never opens a file. Samples that do not
/// fit are counted by @c dropped_samples and are not stored.
/// @c set_format is ignored once any sample has been accepted.
/// @c frame_count and @c data_bytes saturate at @c uint32_t max instead of wrapping.
/// @c write_wav refuses a payload that does not fit a RIFF header and leaves the
/// spill file in place, including after a failed export.
class PcmRecordBuffer {
public:
    static constexpr size_t k_ingress_samples = 131072;

    PcmRecordBuffer();
    ~PcmRecordBuffer();

    PcmRecordBuffer(const PcmRecordBuffer&) = delete;
    PcmRecordBuffer& operator=(const PcmRecordBuffer&) = delete;

    void set_format(uint32_t sample_rate, uint16_t channels);

    /// Pre-reserve RAM for @p seconds of capture. A size that does not fit is skipped.
    void reserve_seconds(uint32_t seconds);

    /// Enable spill-to-disk once RAM holds more than @p seconds of PCM (0 = RAM only).
    void set_spill_threshold_seconds(uint32_t seconds);

    /// @p frame_count is per-channel frames (miniaudio convention).
    void push(const int16_t* pcm, size_t frame_count);

    uint32_t sample_rate() const noexcept;
    uint16_t channels() const noexcept;
    uint32_t frame_count() const noexcept;
    uint32_t data_bytes() const noexcept;
    uint64_t dropped_samples() const noexcept;
    bool     using_spill() const noexcept;

    void clear();

    /// Peak/RMS/clipping stats over captured PCM (incremental when spill is active).
    PcmLevelStats analyze_levels() const;

    /// Write a canonical RIFF PCM WAV to @p dst. The buffer keeps its samples.
    bool write_wav(const std::filesystem::path& dst) const;

private:
    void worker_main();
    void drain_locked(std::unique_lock<std::mutex>& lk) const;
    bool write_spill_block(const std::vector<int16_t>& block);
    void close_spill_locked() const noexcept;
    void remove_spill_file() noexcept;
    uint64_t stored_samples_locked() const noexcept;

    mutable std::mutex mu_;
    mutable std::condition_variable cv_;
    mutable std::condition_variable idle_cv_;
    std::thread worker_;
    bool stop_ = false;
    mutable bool flush_requested_ = false;
    mutable bool exporting_ = false;
    bool busy_ = false;

    std::vector<int16_t> ingress_;
    size_t ingress_r_ = 0;
    size_t ingress_w_ = 0;
    size_t ingress_size_ = 0;
    uint64_t dropped_samples_ = 0;

    std::vector<int16_t> samples_;
    uint32_t             sample_rate_ = 48000;
    uint16_t             channels_    = 2;

    uint32_t spill_threshold_samples_ = 0;
    uint64_t spilled_samples_         = 0;
    mutable PcmLevelStats level_stats_;

    std::filesystem::path spill_path_;
    mutable std::ofstream spill_stream_;
};

} // namespace pm::audio
