#include "pm/audio/pcm_record.hpp"

#include <algorithm>
#include <cmath>
#include <chrono>
#include <fstream>

namespace pm::audio {
namespace {

#pragma pack(push, 1)
struct WavHeader {
    char     riff[4]         = {'R','I','F','F'};
    uint32_t chunk_size      = 0;
    char     wave[4]         = {'W','A','V','E'};
    char     fmt_id[4]       = {'f','m','t',' '};
    uint32_t fmt_size        = 16;
    uint16_t audio_format    = 1;
    uint16_t channels        = 2;
    uint32_t sample_rate     = 48000;
    uint32_t byte_rate       = 0;
    uint16_t block_align     = 0;
    uint16_t bits_per_sample = 16;
    char     data_id[4]      = {'d','a','t','a'};
    uint32_t data_size       = 0;
};
#pragma pack(pop)
static_assert(sizeof(WavHeader) == 44, "WavHeader must be exactly 44 bytes");

constexpr float kInt16FullScale = 32768.0f;

float samples_to_dbfs(float normalized_peak)
{
    if (normalized_peak <= 0.0f) return -100.0f;
    return 20.0f * std::log10(normalized_peak);
}

std::filesystem::path make_spill_path()
{
    namespace fs = std::filesystem;
    std::error_code ec;
    fs::path base = fs::temp_directory_path(ec);
    if (ec) base = fs::path(".");
    const auto tag = std::chrono::steady_clock::now().time_since_epoch().count();
    return base / ("pm-pcm-spill-" + std::to_string(tag) + ".raw");
}

} // namespace

RecordContainer record_container_from_extension(const std::string& ext_lower)
{
    if (ext_lower == ".m4a" || ext_lower == ".aac" || ext_lower == ".mp4")
        return RecordContainer::Aac;
    return RecordContainer::Wav;
}

bool aac_record_supported() noexcept
{
#if defined(_WIN32)
    return true;
#else
    return false;
#endif
}

void accumulate_pcm_levels(PcmLevelStats& stats, const int16_t* samples, size_t sample_count)
{
    if (!samples || sample_count == 0) return;

    int peak = 0;
    if (stats.total_samples > 0) {
        const float prev_peak = std::pow(10.0f, stats.peak_dbfs / 20.0f) * kInt16FullScale;
        peak = static_cast<int>(prev_peak + 0.5f);
    }

    double sum_sq = 0.0;
    if (stats.total_samples > 0) {
        const float prev_rms = std::pow(10.0f, stats.rms_dbfs / 20.0f) * kInt16FullScale;
        sum_sq = static_cast<double>(prev_rms) * static_cast<double>(prev_rms)
                 * static_cast<double>(stats.total_samples);
    }

    for (size_t i = 0; i < sample_count; ++i) {
        const int v = samples[i];
        const int a = v >= 0 ? v : -v;
        if (a > peak) peak = a;
        sum_sq += static_cast<double>(v) * static_cast<double>(v);
        if (a >= 32767) ++stats.clipped_samples;
    }

    stats.total_samples += sample_count;
    stats.peak_dbfs = samples_to_dbfs(static_cast<float>(peak) / kInt16FullScale);
    const double rms = std::sqrt(sum_sq / static_cast<double>(stats.total_samples));
    stats.rms_dbfs   = samples_to_dbfs(static_cast<float>(rms / static_cast<double>(kInt16FullScale)));
}

PcmLevelStats analyze_pcm_levels(const int16_t* samples, size_t sample_count)
{
    PcmLevelStats stats;
    accumulate_pcm_levels(stats, samples, sample_count);
    return stats;
}

void PcmRecordBuffer::set_format(uint32_t sample_rate, uint16_t channels)
{
    std::lock_guard<std::mutex> lk(mu_);
    sample_rate_ = sample_rate > 0 ? sample_rate : 48000;
    channels_    = channels > 0 ? channels : 2;
}

void PcmRecordBuffer::set_spill_threshold_seconds(uint32_t seconds)
{
    std::lock_guard<std::mutex> lk(mu_);
    if (seconds == 0 || channels_ == 0 || sample_rate_ == 0) {
        spill_threshold_samples_ = 0;
        return;
    }
    spill_threshold_samples_ =
        seconds * sample_rate_ * static_cast<uint32_t>(channels_);
}

void PcmRecordBuffer::reserve_seconds(uint32_t seconds)
{
    if (seconds == 0) return;
    std::lock_guard<std::mutex> lk(mu_);
    const size_t frames = static_cast<size_t>(sample_rate_) * seconds;
    samples_.reserve(frames * channels_);
}

void PcmRecordBuffer::push(const int16_t* pcm, size_t frame_count)
{
    if (!pcm || frame_count == 0 || channels_ == 0) return;
    const size_t sample_count = frame_count * static_cast<size_t>(channels_);
    std::lock_guard<std::mutex> lk(mu_);
    accumulate_pcm_levels(level_stats_, pcm, sample_count);
    samples_.insert(samples_.end(), pcm, pcm + sample_count);
    maybe_spill_locked();
}

bool PcmRecordBuffer::ensure_spill_open_locked()
{
    if (spill_stream_.is_open()) return true;
    if (spill_path_.empty())
        spill_path_ = make_spill_path();
    spill_stream_.open(spill_path_, std::ios::binary | std::ios::app);
    return spill_stream_.good();
}

void PcmRecordBuffer::maybe_spill_locked()
{
    if (spill_threshold_samples_ == 0 ||
        samples_.size() < spill_threshold_samples_)
        return;
    if (!ensure_spill_open_locked()) return;

    const auto bytes = static_cast<std::streamsize>(samples_.size() * sizeof(int16_t));
    spill_stream_.write(reinterpret_cast<const char*>(samples_.data()), bytes);
    if (!spill_stream_.good()) return;

    spilled_samples_ += samples_.size();
    samples_.clear();
}

void PcmRecordBuffer::close_spill_locked() const noexcept
{
    if (spill_stream_.is_open()) {
        spill_stream_.flush();
        spill_stream_.close();
    }
}

void PcmRecordBuffer::remove_spill_file() noexcept
{
    close_spill_locked();
    if (spill_path_.empty()) return;
    std::error_code ec;
    std::filesystem::remove(spill_path_, ec);
    spill_path_.clear();
}

uint32_t PcmRecordBuffer::frame_count() const noexcept
{
    std::lock_guard<std::mutex> lk(mu_);
    if (channels_ == 0) return 0;
    const uint64_t total_samples = spilled_samples_ + samples_.size();
    return static_cast<uint32_t>(total_samples / channels_);
}

uint32_t PcmRecordBuffer::data_bytes() const noexcept
{
    std::lock_guard<std::mutex> lk(mu_);
    const uint64_t total_samples = spilled_samples_ + samples_.size();
    return static_cast<uint32_t>(total_samples * sizeof(int16_t));
}

bool PcmRecordBuffer::using_spill() const noexcept
{
    std::lock_guard<std::mutex> lk(mu_);
    return spilled_samples_ > 0 || spill_stream_.is_open();
}

void PcmRecordBuffer::clear()
{
    std::lock_guard<std::mutex> lk(mu_);
    samples_.clear();
    spilled_samples_ = 0;
    level_stats_     = {};
    remove_spill_file();
}

PcmLevelStats PcmRecordBuffer::analyze_levels() const
{
    std::lock_guard<std::mutex> lk(mu_);
    return level_stats_;
}

bool PcmRecordBuffer::write_wav(const std::filesystem::path& dst) const
{
    std::vector<int16_t> tail;
    uint32_t rate = 0;
    uint16_t ch   = 0;
    uint64_t spilled = 0;
    std::filesystem::path spill_copy;
    {
        std::lock_guard<std::mutex> lk(mu_);
        tail     = samples_;
        rate     = sample_rate_;
        ch       = channels_;
        spilled  = spilled_samples_;
        spill_copy = spill_path_;
        close_spill_locked();
    }

    if (ch == 0 || rate == 0) return false;

    const uint64_t total_samples = spilled + tail.size();
    if (total_samples == 0) return false;

    std::error_code ec;
    if (dst.has_parent_path())
        std::filesystem::create_directories(dst.parent_path(), ec);

    std::ofstream out(dst, std::ios::binary | std::ios::trunc);
    if (!out) return false;

    const uint32_t data_bytes =
        static_cast<uint32_t>(total_samples * sizeof(int16_t));
    WavHeader hdr{};
    hdr.channels        = ch;
    hdr.sample_rate     = rate;
    hdr.byte_rate       = rate * ch * (hdr.bits_per_sample / 8u);
    hdr.block_align     = static_cast<uint16_t>(ch * (hdr.bits_per_sample / 8u));
    hdr.data_size       = data_bytes;
    hdr.chunk_size      = data_bytes + 36;

    out.write(reinterpret_cast<const char*>(&hdr), sizeof(hdr));

    if (spilled > 0 && !spill_copy.empty()) {
        std::ifstream spill_in(spill_copy, std::ios::binary);
        if (!spill_in) return false;
        std::vector<char> chunk(256 * 1024);
        while (spill_in) {
            spill_in.read(chunk.data(), static_cast<std::streamsize>(chunk.size()));
            const auto got = spill_in.gcount();
            if (got > 0)
                out.write(chunk.data(), got);
        }
    }

    if (!tail.empty()) {
        out.write(reinterpret_cast<const char*>(tail.data()),
                  static_cast<std::streamsize>(tail.size() * sizeof(int16_t)));
    }

    out.flush();
    const bool ok = out.good();

    if (!spill_copy.empty()) {
        std::error_code rm_ec;
        std::filesystem::remove(spill_copy, rm_ec);
    }
    return ok;
}

} // namespace pm::audio
