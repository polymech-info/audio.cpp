#include "pm/audio/pcm_record.hpp"

#include <algorithm>
#include <cmath>
#include <chrono>
#include <fstream>
#include <utility>
#include <vector>

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
constexpr size_t k_reserve_cap_samples = 256ull * 1024ull * 1024ull;

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
    if (ec) base = ".";
    const auto tag = std::chrono::steady_clock::now().time_since_epoch().count();
    return base / ("pm-pcm-spill-" + std::to_string(tag) + ".raw");
}

uint32_t saturate_u32(uint64_t value)
{
    if (value > std::numeric_limits<uint32_t>::max())
        return std::numeric_limits<uint32_t>::max();
    return static_cast<uint32_t>(value);
}

bool write_all(std::ostream& out, const char* data, uint64_t bytes)
{
    constexpr uint64_t k_chunk = 1024ull * 1024ull;
    uint64_t done = 0;
    while (done < bytes) {
        const uint64_t n = std::min(k_chunk, bytes - done);
        out.write(data + done, static_cast<std::streamsize>(n));
        if (!out)
            return false;
        done += n;
    }
    return true;
}

} // namespace

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

PcmRecordBuffer::PcmRecordBuffer()
    : ingress_(k_ingress_samples)
{
    worker_ = std::thread([this] { worker_main(); });
}

PcmRecordBuffer::~PcmRecordBuffer()
{
    {
        std::lock_guard<std::mutex> lk(mu_);
        stop_ = true;
        flush_requested_ = false;
    }
    cv_.notify_all();
    if (worker_.joinable())
        worker_.join();
    remove_spill_file();
}

uint64_t PcmRecordBuffer::stored_samples_locked() const noexcept
{
    return spilled_samples_ + samples_.size() + ingress_size_;
}

void PcmRecordBuffer::set_format(uint32_t sample_rate, uint16_t channels)
{
    std::lock_guard<std::mutex> lk(mu_);
    if (busy_ || ingress_size_ != 0 || spilled_samples_ != 0 || !samples_.empty())
        return;
    sample_rate_ = sample_rate > 0 ? sample_rate : 48000;
    channels_    = channels > 0 ? channels : 2;
}

void PcmRecordBuffer::set_spill_threshold_seconds(uint32_t seconds)
{
    std::lock_guard<std::mutex> lk(mu_);
    spill_threshold_samples_ = spill_sample_threshold(seconds, sample_rate_, channels_);
}

void PcmRecordBuffer::reserve_seconds(uint32_t seconds)
{
    if (seconds == 0) return;
    std::lock_guard<std::mutex> lk(mu_);
    if (sample_rate_ == 0 || channels_ == 0) return;
    if (static_cast<uint64_t>(seconds) > std::numeric_limits<size_t>::max() / sample_rate_)
        return;
    const size_t frames = static_cast<size_t>(sample_rate_) * static_cast<size_t>(seconds);
    if (frames > std::numeric_limits<size_t>::max() / channels_)
        return;
    const size_t n = frames * static_cast<size_t>(channels_);
    if (n > k_reserve_cap_samples || n > samples_.max_size())
        return;
    samples_.reserve(n);
}

void PcmRecordBuffer::push(const int16_t* pcm, size_t frame_count)
{
    if (!pcm || frame_count == 0) return;
    {
        std::lock_guard<std::mutex> lk(mu_);
        if (channels_ == 0) return;
        if (frame_count > std::numeric_limits<size_t>::max() / channels_) {
            dropped_samples_ += frame_count;
            return;
        }
        size_t left = frame_count * static_cast<size_t>(channels_);
        const int16_t* src = pcm;
        while (left > 0) {
            if (ingress_size_ == ingress_.size()) {
                dropped_samples_ += left;
                break;
            }
            const size_t space = ingress_.size() - ingress_size_;
            const size_t n = std::min(left, space);
            for (size_t i = 0; i < n; ++i) {
                ingress_[ingress_w_] = src[i];
                ingress_w_ = (ingress_w_ + 1) % ingress_.size();
            }
            ingress_size_ += n;
            src += n;
            left -= n;
        }
    }
    cv_.notify_one();
}

uint32_t PcmRecordBuffer::sample_rate() const noexcept
{
    std::lock_guard<std::mutex> lk(mu_);
    return sample_rate_;
}

uint16_t PcmRecordBuffer::channels() const noexcept
{
    std::lock_guard<std::mutex> lk(mu_);
    return channels_;
}

uint32_t PcmRecordBuffer::frame_count() const noexcept
{
    std::lock_guard<std::mutex> lk(mu_);
    if (channels_ == 0) return 0;
    return saturate_u32(stored_samples_locked() / channels_);
}

uint32_t PcmRecordBuffer::data_bytes() const noexcept
{
    std::lock_guard<std::mutex> lk(mu_);
    const uint64_t samples = stored_samples_locked();
    if (samples > std::numeric_limits<uint64_t>::max() / sizeof(int16_t))
        return std::numeric_limits<uint32_t>::max();
    return saturate_u32(samples * sizeof(int16_t));
}

uint64_t PcmRecordBuffer::dropped_samples() const noexcept
{
    std::lock_guard<std::mutex> lk(mu_);
    return dropped_samples_;
}

bool PcmRecordBuffer::using_spill() const noexcept
{
    std::lock_guard<std::mutex> lk(mu_);
    return spilled_samples_ > 0 || spill_stream_.is_open();
}

void PcmRecordBuffer::clear()
{
    std::unique_lock<std::mutex> lk(mu_);
    drain_locked(lk);
    samples_.clear();
    spilled_samples_ = 0;
    dropped_samples_ = 0;
    level_stats_ = {};
    exporting_ = false;
    remove_spill_file();
}

PcmLevelStats PcmRecordBuffer::analyze_levels() const
{
    std::lock_guard<std::mutex> lk(mu_);
    return level_stats_;
}

void PcmRecordBuffer::drain_locked(std::unique_lock<std::mutex>& lk) const
{
    flush_requested_ = true;
    cv_.notify_all();
    idle_cv_.wait(lk, [&] { return ingress_size_ == 0 && !busy_; });
    exporting_ = true;
    flush_requested_ = false;
    cv_.notify_all();
}

bool PcmRecordBuffer::write_spill_block(const std::vector<int16_t>& block)
{
    if (block.empty()) return true;
    if (spill_path_.empty())
        spill_path_ = make_spill_path();
    if (!spill_stream_.is_open()) {
        spill_stream_.open(spill_path_, std::ios::binary | std::ios::app);
        if (!spill_stream_)
            return false;
    }
    const uint64_t bytes = static_cast<uint64_t>(block.size()) * sizeof(int16_t);
    return write_all(spill_stream_, reinterpret_cast<const char*>(block.data()), bytes);
}

void PcmRecordBuffer::worker_main()
{
    std::unique_lock<std::mutex> lk(mu_);
    while (true) {
        cv_.wait(lk, [&] { return stop_ || ingress_size_ > 0 || flush_requested_; });
        if (ingress_size_ == 0) {
            busy_ = false;
            idle_cv_.notify_all();
            if (stop_)
                break;
            cv_.wait(lk, [&] { return stop_ || ingress_size_ > 0 || !flush_requested_; });
            continue;
        }

        std::vector<int16_t> chunk(ingress_size_);
        for (size_t i = 0; i < chunk.size(); ++i) {
            chunk[i] = ingress_[ingress_r_];
            ingress_r_ = (ingress_r_ + 1) % ingress_.size();
        }
        ingress_size_ = 0;
        busy_ = true;

        try {
            accumulate_pcm_levels(level_stats_, chunk.data(), chunk.size());
            samples_.insert(samples_.end(), chunk.begin(), chunk.end());
        } catch (...) {
            busy_ = false;
            idle_cv_.notify_all();
            continue;
        }

        if (exporting_ || spill_threshold_samples_ == 0 ||
            samples_.size() < spill_threshold_samples_) {
            busy_ = false;
            idle_cv_.notify_all();
            continue;
        }

        std::vector<int16_t> block = std::move(samples_);
        samples_.clear();
        lk.unlock();
        bool wrote = false;
        try {
            wrote = write_spill_block(block);
        } catch (...) {
            wrote = false;
        }
        lk.lock();
        if (wrote)
            spilled_samples_ += block.size();
        else {
            block.insert(block.end(), samples_.begin(), samples_.end());
            samples_ = std::move(block);
        }
        busy_ = false;
        idle_cv_.notify_all();
    }
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
    spilled_samples_ = 0;
}

bool PcmRecordBuffer::write_wav(const std::filesystem::path& dst) const
{
    std::vector<int16_t> tail;
    uint32_t rate = 0;
    uint16_t ch = 0;
    uint64_t spilled = 0;
    std::filesystem::path spill;
    {
        std::unique_lock<std::mutex> lk(mu_);
        drain_locked(lk);
        close_spill_locked();
        tail = samples_;
        rate = sample_rate_;
        ch = channels_;
        spilled = spilled_samples_;
        spill = spill_path_;
    }

    auto finish = [&](bool) {
        std::lock_guard<std::mutex> lk(mu_);
        exporting_ = false;
        cv_.notify_all();
    };

    if (ch == 0 || rate == 0) {
        finish(false);
        return false;
    }
    const uint64_t total_samples = spilled + tail.size();
    if (!pcm_wav_header_fits(total_samples, rate, ch)) {
        finish(false);
        return false;
    }

    std::error_code ec;
    if (dst.has_parent_path())
        std::filesystem::create_directories(dst.parent_path(), ec);

    std::ofstream out(dst, std::ios::binary | std::ios::trunc);
    if (!out) {
        finish(false);
        return false;
    }

    const uint32_t data_bytes = static_cast<uint32_t>(total_samples * sizeof(int16_t));
    WavHeader hdr{};
    hdr.channels = ch;
    hdr.sample_rate = rate;
    hdr.byte_rate = static_cast<uint32_t>(static_cast<uint64_t>(rate) * ch * 2u);
    hdr.block_align = static_cast<uint16_t>(ch * 2u);
    hdr.data_size = data_bytes;
    hdr.chunk_size = data_bytes + 36;
    if (!write_all(out, reinterpret_cast<const char*>(&hdr), sizeof(hdr))) {
        finish(false);
        return false;
    }

    if (spilled > 0) {
        if (spill.empty()) {
            finish(false);
            return false;
        }
        std::ifstream spill_in(spill, std::ios::binary);
        if (!spill_in) {
            finish(false);
            return false;
        }
        const uint64_t want = spilled * sizeof(int16_t);
        uint64_t got_bytes = 0;
        std::vector<char> chunk(256 * 1024);
        while (got_bytes < want) {
            const uint64_t ask = std::min<uint64_t>(chunk.size(), want - got_bytes);
            spill_in.read(chunk.data(), static_cast<std::streamsize>(ask));
            const auto got = spill_in.gcount();
            if (got <= 0) {
                finish(false);
                return false;
            }
            if (!write_all(out, chunk.data(), static_cast<uint64_t>(got))) {
                finish(false);
                return false;
            }
            got_bytes += static_cast<uint64_t>(got);
        }
    }

    if (!tail.empty()) {
        const uint64_t bytes = static_cast<uint64_t>(tail.size()) * sizeof(int16_t);
        if (!write_all(out, reinterpret_cast<const char*>(tail.data()), bytes)) {
            finish(false);
            return false;
        }
    }

    out.flush();
    const bool ok = static_cast<bool>(out);
    finish(ok);
    return ok;
}

} // namespace pm::audio
