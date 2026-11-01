#pragma once

#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <mutex>
#include <string>
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

/// Thread-safe PCM accumulator for record paths. Audio callbacks should only call push().
/// When spill is enabled, PCM is flushed to a temp raw file once RAM exceeds the threshold.
class PcmRecordBuffer {
public:
    void set_format(uint32_t sample_rate, uint16_t channels);

    /// Pre-reserve RAM for @p seconds of capture (reduces realloc in the audio thread).
    void reserve_seconds(uint32_t seconds);

    /// Enable spill-to-disk once RAM holds more than @p seconds of PCM (0 = RAM only).
    void set_spill_threshold_seconds(uint32_t seconds);

    /// @p frame_count is per-channel frames (miniaudio convention).
    void push(const int16_t* pcm, size_t frame_count);

    uint32_t sample_rate() const noexcept { return sample_rate_; }
    uint16_t channels() const noexcept { return channels_; }
    uint32_t frame_count() const noexcept;
    uint32_t data_bytes() const noexcept;
    bool     using_spill() const noexcept;

    void clear();

    /// Peak/RMS/clipping stats over captured PCM (incremental when spill is active).
    PcmLevelStats analyze_levels() const;

    /// Write a canonical RIFF PCM WAV to @p dst.
    bool write_wav(const std::filesystem::path& dst) const;

private:
    void maybe_spill_locked();
    bool ensure_spill_open_locked();
    void close_spill_locked() const noexcept;
    void remove_spill_file() noexcept;

    mutable std::mutex   mu_;
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
