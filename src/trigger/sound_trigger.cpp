#include "pm/audio/trigger/sound_trigger.hpp"

#include "pm/audio/trigger/sound_trigger_whistle.hpp"

#include "pm/audio/log.hpp"

#include <pffft/pffft.h>
#include <nlohmann/json.hpp>

#include <algorithm>
#include <array>
#include <cmath>
#include <cstring>
#include <fstream>
#include <iomanip>
#include <limits>
#include <memory>
#include <numeric>
#include <sstream>
#include <type_traits>
#include <utility>

namespace pm::audio::trigger {
namespace {

constexpr std::size_t k_calibration_frames = 20;
constexpr std::size_t k_event_end_quiet_frames = 12;
constexpr std::size_t k_min_template_frames = 5;
constexpr std::size_t k_max_wav_bytes = 64U * 1024U * 1024U;
constexpr float k_pi = 3.14159265358979323846f;
constexpr float k_inf = std::numeric_limits<float>::infinity();

std::uint16_t u16le(const unsigned char* p)
{
    return static_cast<std::uint16_t>(p[0])
        | static_cast<std::uint16_t>(p[1] << 8);
}

std::uint32_t u32le(const unsigned char* p)
{
    return static_cast<std::uint32_t>(p[0])
        | (static_cast<std::uint32_t>(p[1]) << 8)
        | (static_cast<std::uint32_t>(p[2]) << 16)
        | (static_cast<std::uint32_t>(p[3]) << 24);
}

void write_u32le(std::ostream& out, std::uint32_t value)
{
    const std::array<unsigned char, 4> bytes{
        static_cast<unsigned char>(value & 0xffU),
        static_cast<unsigned char>((value >> 8) & 0xffU),
        static_cast<unsigned char>((value >> 16) & 0xffU),
        static_cast<unsigned char>((value >> 24) & 0xffU),
    };
    out.write(reinterpret_cast<const char*>(bytes.data()), bytes.size());
}

bool read_u32le(std::istream& in, std::uint32_t& value)
{
    std::array<unsigned char, 4> bytes{};
    if (!in.read(reinterpret_cast<char*>(bytes.data()), bytes.size()))
        return false;
    value = u32le(bytes.data());
    return true;
}

void write_f32le(std::ostream& out, float value)
{
    static_assert(sizeof(float) == sizeof(std::uint32_t));
    static_assert(std::numeric_limits<float>::is_iec559);
    std::uint32_t bits = 0;
    std::memcpy(&bits, &value, sizeof(bits));
    write_u32le(out, bits);
}

bool read_f32le(std::istream& in, float& value)
{
    std::uint32_t bits = 0;
    if (!read_u32le(in, bits))
        return false;
    std::memcpy(&value, &bits, sizeof(value));
    return true;
}

class Spectrum {
public:
    struct SetupDeleter {
        void operator()(PFFFT_Setup* value) const noexcept
        {
            if (value)
                pffft_destroy_setup(value);
        }
    };
    struct AlignedDeleter {
        void operator()(float* value) const noexcept
        {
            if (value)
                pffft_aligned_free(value);
        }
    };

    Spectrum()
        : setup_(pffft_new_setup(static_cast<int>(k_fft_size), PFFFT_REAL)),
          input_(allocate()),
          output_(allocate()),
          work_(allocate())
    {
        if (!setup_ || !input_ || !output_ || !work_)
            throw std::bad_alloc();
        for (std::size_t i = 0; i < k_fft_size; ++i)
            window_[i] = (0.5f - 0.5f * std::cos(
                2.0f * k_pi * static_cast<float>(i)
                / static_cast<float>(k_fft_size - 1))) / 32768.0f;
    }

    Spectrum(const Spectrum&) = delete;
    Spectrum& operator=(const Spectrum&) = delete;

    void power(const std::int16_t* pcm, SpectrumBins& bins)
    {
        for (std::size_t i = 0; i < k_fft_size; ++i)
            input_.get()[i] = static_cast<float>(pcm[i]) * window_[i];
        pffft_transform_ordered(setup_.get(), input_.get(), output_.get(),
                                work_.get(), PFFFT_FORWARD);
        bins[0] = output_.get()[0] * output_.get()[0];
        bins[k_fft_size / 2] = output_.get()[1] * output_.get()[1];
        for (std::size_t k = 1; k < k_fft_size / 2; ++k) {
            const float re = output_.get()[2 * k];
            const float im = output_.get()[2 * k + 1];
            bins[k] = re * re + im * im;
        }
    }

private:
    static std::unique_ptr<float, AlignedDeleter> allocate()
    {
        return std::unique_ptr<float, AlignedDeleter>(
            static_cast<float*>(
                pffft_aligned_malloc(k_fft_size * sizeof(float))));
    }

    std::unique_ptr<PFFFT_Setup, SetupDeleter> setup_;
    std::unique_ptr<float, AlignedDeleter> input_;
    std::unique_ptr<float, AlignedDeleter> output_;
    std::unique_ptr<float, AlignedDeleter> work_;
    std::array<float, k_fft_size> window_{};
};

float activity_threshold(float noise_rms) noexcept
{
    return std::max(120.0f, noise_rms * 2.2f);
}

float frame_rms(const std::int16_t* pcm, std::size_t count)
{
    if (!pcm || count == 0)
        return 0.0f;
    double sum = 0.0;
    for (std::size_t i = 0; i < count; ++i) {
        const double v = static_cast<double>(pcm[i]);
        sum += v * v;
    }
    return static_cast<float>(std::sqrt(sum / static_cast<double>(count)));
}

FrameMetrics basic_metrics(const std::int16_t* pcm, float noise_rms,
                           std::uint64_t timestamp_ms)
{
    FrameMetrics m;
    m.timestamp_ms = timestamp_ms;
    m.rms = frame_rms(pcm, k_fft_size);
    m.noise_rms = noise_rms;
    m.active = m.rms >= activity_threshold(noise_rms);
    return m;
}

using FeatureRanges =
    std::array<std::pair<std::size_t, std::size_t>, k_feature_bins>;

const FeatureRanges& feature_ranges()
{
    static const FeatureRanges ranges = [] {
        FeatureRanges out{};
        constexpr float min_hz = 80.0f;
        constexpr float max_hz = k_default_sample_rate * 0.475f;
        const float min_log = std::log(min_hz);
        const float range = std::log(max_hz) - min_log;
        for (std::size_t b = 0; b < k_feature_bins; ++b) {
            const float lo_hz =
                std::exp(min_log + range * b / k_feature_bins);
            const float hi_hz =
                std::exp(min_log + range * (b + 1) / k_feature_bins);
            const std::size_t lo = std::max<std::size_t>(
                1, static_cast<std::size_t>(
                    lo_hz * k_fft_size / k_default_sample_rate));
            const std::size_t hi = std::min<std::size_t>(
                k_spectrum_bins - 1,
                std::max(lo, static_cast<std::size_t>(
                    hi_hz * k_fft_size / k_default_sample_rate)));
            out[b] = {lo, hi};
        }
        return out;
    }();
    return ranges;
}

FeatureFrame log_band_features(const SpectrumBins& power)
{
    FeatureFrame out{};
    const auto& ranges = feature_ranges();
    for (std::size_t b = 0; b < k_feature_bins; ++b) {
        const auto [lo, hi] = ranges[b];
        float energy = 0.0f;
        for (std::size_t k = lo; k <= hi; ++k)
            energy += std::max(power[k], 0.0f);
        out[b] = std::log1p(energy);
    }
    const float mean = std::accumulate(out.begin(), out.end(), 0.0f)
        / static_cast<float>(k_feature_bins);
    float variance = 0.0f;
    for (float v : out)
        variance += (v - mean) * (v - mean);
    const float scale = std::sqrt(
        variance / static_cast<float>(k_feature_bins) + 1e-6f);
    for (float& v : out)
        v = (v - mean) / scale;
    return out;
}

float estimate_noise_rms(const std::int16_t* pcm, std::size_t samples,
                         std::uint32_t sample_rate)
{
    const std::size_t window = std::max<std::size_t>(1, sample_rate / 100);
    std::vector<float> levels;
    for (std::size_t offset = 0; offset + window <= samples; offset += window)
        levels.push_back(frame_rms(pcm + offset, window));
    if (levels.empty())
        return 20.0f;
    const std::size_t index = levels.size() / 5;
    std::nth_element(levels.begin(), levels.begin() + index, levels.end());
    return std::max(20.0f, levels[index]);
}

struct EventBuilder {
    enum class Status { idle, open, ready, rejected };

    TemplateSequence sequence;
    bool open = false;
    bool suppressed = false;
    bool rejected = false;
    std::size_t quiet = 0;

    bool needs_features(bool frame_active) const noexcept
    {
        if (rejected)
            return false;
        if (open)
            return true;
        return frame_active && !suppressed;
    }

    Status observe(bool frame_active, const FeatureFrame* feature)
    {
        if (rejected) {
            if (!frame_active) {
                rejected = false;
                suppressed = false;
            }
            return Status::idle;
        }
        if (frame_active && !suppressed) {
            if (!open) {
                open = true;
                quiet = 0;
                sequence.frames.clear();
                sequence.frames.reserve(64);
            }
            quiet = 0;
        } else if (open) {
            ++quiet;
        } else {
            if (!frame_active)
                suppressed = false;
            return Status::idle;
        }
        if (feature == nullptr
            || sequence.frames.size() >= k_max_template_frames) {
            sequence.frames.clear();
            open = false;
            suppressed = true;
            rejected = true;
            quiet = 0;
            return Status::rejected;
        }
        sequence.frames.push_back(*feature);
        if (quiet < k_event_end_quiet_frames)
            return Status::open;
        if (quiet <= sequence.frames.size())
            sequence.frames.resize(sequence.frames.size() - quiet);
        else
            sequence.frames.clear();
        quiet = 0;
        open = false;
        if (sequence.frames.size() < k_min_template_frames) {
            sequence.frames.clear();
            return Status::idle;
        }
        return Status::ready;
    }

    bool finish()
    {
        if (rejected) {
            sequence.frames.clear();
            return false;
        }
        if (open) {
            if (quiet > 0 && quiet <= sequence.frames.size())
                sequence.frames.resize(sequence.frames.size() - quiet);
            quiet = 0;
            open = false;
        }
        if (sequence.frames.size() < k_min_template_frames) {
            sequence.frames.clear();
            return false;
        }
        return true;
    }
};

TemplateSequence event_features(const std::int16_t* pcm, std::size_t samples,
                                std::uint32_t sample_rate)
{
    TemplateSequence empty;
    if (!pcm || sample_rate != k_default_sample_rate || samples < k_fft_size)
        return empty;
    const float threshold = activity_threshold(
        estimate_noise_rms(pcm, samples, sample_rate));
    EventBuilder builder;
    Spectrum fft;
    SpectrumBins power{};
    for (std::size_t offset = 0; offset <= samples - k_fft_size;
         offset += k_hop_size) {
        const bool active =
            frame_rms(pcm + offset, k_fft_size) >= threshold;
        FeatureFrame frame{};
        const FeatureFrame* feature = nullptr;
        if (builder.needs_features(active)) {
            fft.power(pcm + offset, power);
            frame = log_band_features(power);
            feature = &frame;
        }
        const auto status = builder.observe(active, feature);
        if (status == EventBuilder::Status::ready)
            return std::move(builder.sequence);
        if (status == EventBuilder::Status::rejected)
            return empty;
    }
    if (!builder.finish())
        return empty;
    return std::move(builder.sequence);
}

float feature_distance(const FeatureFrame& a, const FeatureFrame& b)
{
    float sum = 0.0f;
    for (std::size_t i = 0; i < k_feature_bins; ++i) {
        const float d = a[i] - b[i];
        sum += d * d;
    }
    return std::sqrt(sum / static_cast<float>(k_feature_bins));
}

nlohmann::json whistle_json(const WhistleConfig& w)
{
    return {
        {"min_hz", w.min_hz}, {"max_hz", w.max_hz},
        {"min_concentration", w.min_concentration},
        {"max_flatness", w.max_flatness},
        {"min_band_ratio", w.min_band_ratio},
        {"max_pitch_jump_hz", w.max_pitch_jump_hz},
        {"min_duration_ms", w.min_duration_ms},
        {"max_duration_ms", w.max_duration_ms},
        {"min_gap_ms", w.min_gap_ms}, {"max_gap_ms", w.max_gap_ms},
        {"cooldown_ms", w.cooldown_ms},
    };
}

void read_whistle(const nlohmann::json& j, WhistleConfig& w)
{
    w.min_hz = j.value("min_hz", w.min_hz);
    w.max_hz = j.value("max_hz", w.max_hz);
    w.min_concentration = j.value("min_concentration", w.min_concentration);
    w.max_flatness = j.value("max_flatness", w.max_flatness);
    w.min_band_ratio = j.value("min_band_ratio", w.min_band_ratio);
    w.max_pitch_jump_hz = j.value("max_pitch_jump_hz", w.max_pitch_jump_hz);
    w.min_duration_ms = j.value("min_duration_ms", w.min_duration_ms);
    w.max_duration_ms = j.value("max_duration_ms", w.max_duration_ms);
    w.min_gap_ms = j.value("min_gap_ms", w.min_gap_ms);
    w.max_gap_ms = j.value("max_gap_ms", w.max_gap_ms);
    w.cooldown_ms = j.value("cooldown_ms", w.cooldown_ms);
}

bool validate_profile(const Profile& p, std::string& error)
{
    if (p.schema_version != k_profile_schema_version
        || p.feature_version != k_feature_version) {
        error = "unsupported sound-trigger profile schema";
        return false;
    }
    if (p.trigger_id.empty()) {
        error = "sound-trigger profile has an empty trigger id";
        return false;
    }
    if (p.sample_rate != k_default_sample_rate || p.fft_size != k_fft_size
        || p.hop_size != k_hop_size || p.feature_bins != k_feature_bins) {
        error = "sound-trigger profile uses incompatible feature settings";
        return false;
    }
    if (p.detector != "double-whistle" && p.detector != "template-dtw") {
        error = "unknown sound-trigger detector";
        return false;
    }
    if (p.detector == "template-dtw" && p.templates.empty()) {
        error = "template profile has no templates";
        return false;
    }
    if (p.templates.size() > k_max_templates) {
        error = "sound-trigger profile has too many templates";
        return false;
    }
    for (const auto& sequence : p.templates) {
        if (sequence.frames.empty()
            || sequence.frames.size() > k_max_template_frames) {
            error = "sound-trigger profile has an invalid template length";
            return false;
        }
        for (const auto& frame : sequence.frames) {
            if (!std::all_of(frame.begin(), frame.end(),
                             [](float value) { return std::isfinite(value); })) {
                error = "sound-trigger profile contains a non-finite feature";
                return false;
            }
        }
    }
    if (p.detector == "template-dtw"
        && (!std::isfinite(p.threshold) || p.threshold <= 0.0f
            || !std::isfinite(p.margin) || p.margin < 0.0f)) {
        error = "template profile has invalid threshold or margin";
        return false;
    }
    const auto& w = p.whistle;
    if (!std::isfinite(w.min_hz) || !std::isfinite(w.max_hz)
        || w.min_hz <= 0.0f || w.min_hz >= w.max_hz
        || !std::isfinite(w.min_concentration)
        || w.min_concentration <= 0.0f || w.min_concentration > 1.0f
        || !std::isfinite(w.max_flatness)
        || w.max_flatness <= 0.0f || w.max_flatness > 1.0f
        || !std::isfinite(w.min_band_ratio) || w.min_band_ratio <= 0.0f
        || w.min_band_ratio > 100.0f
        || !std::isfinite(w.max_pitch_jump_hz) || w.max_pitch_jump_hz <= 0.0f
        || w.max_hz >= static_cast<float>(p.sample_rate) * 0.5f
        || w.min_duration_ms == 0 || w.min_duration_ms > w.max_duration_ms
        || w.max_duration_ms > 5000
        || w.min_gap_ms > w.max_gap_ms || w.max_gap_ms > 5000
        || w.cooldown_ms == 0 || w.cooldown_ms > 60000) {
        error = "sound-trigger profile has invalid whistle parameters";
        return false;
    }
    return true;
}

std::uint64_t feature_hash(const std::vector<TemplateSequence>& templates)
{
    std::uint64_t hash = 1469598103934665603ULL;
    auto add_u32 = [&](std::uint32_t value) {
        for (unsigned shift = 0; shift < 32; shift += 8) {
            hash ^= static_cast<unsigned char>((value >> shift) & 0xffU);
            hash *= 1099511628211ULL;
        }
    };
    for (const auto& t : templates) {
        const std::uint32_t frames = static_cast<std::uint32_t>(t.frames.size());
        add_u32(frames);
        for (const auto& frame : t.frames) {
            for (float value : frame) {
                std::uint32_t bits = 0;
                std::memcpy(&bits, &value, sizeof(bits));
                add_u32(bits);
            }
        }
    }
    return hash;
}

std::string hex_hash(std::uint64_t value)
{
    std::ostringstream out;
    out << std::hex << std::setw(16) << std::setfill('0') << value;
    return out.str();
}

std::vector<std::int16_t> resample_to_detector_rate(
    const std::vector<std::int16_t>& input, std::uint32_t sample_rate)
{
    if (sample_rate == k_default_sample_rate || input.empty())
        return input;
    const double step = static_cast<double>(sample_rate)
        / static_cast<double>(k_default_sample_rate);
    const auto out_count = static_cast<std::size_t>(
        static_cast<double>(input.size()) / step);
    std::vector<std::int16_t> output(out_count);
    for (std::size_t i = 0; i < out_count; ++i) {
        double value = 0.0;
        if (step >= 1.0) {
            const double center = (static_cast<double>(i) + 0.5) * step;
            const double half = step * 0.5;
            const auto lo = center > half
                ? static_cast<std::size_t>(center - half) : 0;
            auto hi = static_cast<std::size_t>(std::min(
                static_cast<double>(input.size()), center + half));
            if (hi <= lo)
                hi = std::min(input.size(), lo + 1);
            double sum = 0.0;
            for (std::size_t k = lo; k < hi; ++k)
                sum += static_cast<double>(input[k]);
            value = sum / static_cast<double>(hi - lo);
        } else {
            const double pos = static_cast<double>(i) * step;
            const auto index = static_cast<std::size_t>(pos);
            const double frac = pos - static_cast<double>(index);
            const auto next = std::min(index + 1, input.size() - 1);
            value = static_cast<double>(input[index]) * (1.0 - frac)
                + static_cast<double>(input[next]) * frac;
        }
        const auto rounded = std::llround(value);
        output[i] = static_cast<std::int16_t>(std::clamp(
            rounded, static_cast<long long>(-32768),
            static_cast<long long>(32767)));
    }
    return output;
}

} // namespace

bool read_wav_mono16(const std::filesystem::path& path, WavData& out,
                     std::string& error)
{
    out = {};
    std::error_code size_error;
    const auto file_size = std::filesystem::file_size(path, size_error);
    if (size_error || file_size < 44 || file_size > k_max_wav_bytes) {
        error = "WAV is missing, too small, or exceeds 64 MiB";
        return false;
    }
    std::ifstream in(path, std::ios::binary);
    if (!in) {
        error = "cannot open WAV: " + path.string();
        return false;
    }
    unsigned char header[12]{};
    if (!in.read(reinterpret_cast<char*>(header), sizeof(header))
        || std::memcmp(header, "RIFF", 4) != 0
        || std::memcmp(header + 8, "WAVE", 4) != 0) {
        error = "input is not a RIFF/WAVE file";
        return false;
    }
    std::uint16_t format = 0;
    std::uint16_t channels = 0;
    std::uint16_t bits = 0;
    std::vector<unsigned char> data;
    while (in) {
        unsigned char chunk[8]{};
        if (!in.read(reinterpret_cast<char*>(chunk), sizeof(chunk)))
            break;
        const std::uint32_t size = u32le(chunk + 4);
        if (size > k_max_wav_bytes) {
            error = "WAV chunk exceeds the supported size";
            return false;
        }
        if (std::memcmp(chunk, "fmt ", 4) == 0) {
            std::array<unsigned char, 40> fmt{};
            const std::size_t keep = std::min<std::size_t>(size, fmt.size());
            if (size < 16
                || !in.read(reinterpret_cast<char*>(fmt.data()), keep)) {
                error = "invalid WAV fmt chunk";
                return false;
            }
            if (size > keep)
                in.seekg(static_cast<std::streamoff>(size - keep), std::ios::cur);
            format = u16le(fmt.data());
            channels = u16le(fmt.data() + 2);
            out.sample_rate = u32le(fmt.data() + 4);
            bits = u16le(fmt.data() + 14);
        } else if (std::memcmp(chunk, "data", 4) == 0) {
            data.resize(size);
            if (!in.read(reinterpret_cast<char*>(data.data()), size)) {
                error = "truncated WAV data";
                return false;
            }
        } else {
            in.seekg(size, std::ios::cur);
        }
        if (size & 1)
            in.seekg(1, std::ios::cur);
    }
    if (format != 1 || bits != 16 || (channels != 1 && channels != 2)
        || out.sample_rate == 0 || data.empty()) {
        error = "WAV must be PCM16 mono or stereo";
        return false;
    }
    const std::size_t frame_bytes = channels * sizeof(std::int16_t);
    if (data.size() % frame_bytes != 0) {
        error = "WAV data ends with an incomplete PCM frame";
        return false;
    }
    const std::size_t frames = data.size() / frame_bytes;
    out.mono.resize(frames);
    for (std::size_t i = 0; i < frames; ++i) {
        const std::size_t byte = i * channels * sizeof(std::int16_t);
        const auto left = static_cast<std::int16_t>(u16le(data.data() + byte));
        if (channels == 1) {
            out.mono[i] = left;
        } else {
            const auto right = static_cast<std::int16_t>(
                u16le(data.data() + byte + sizeof(std::int16_t)));
            out.mono[i] = static_cast<std::int16_t>(
                (static_cast<int>(left) + right) / 2);
        }
    }
    if (out.sample_rate != k_default_sample_rate) {
        if (out.sample_rate < 8000 || out.sample_rate > 192000) {
            error = "WAV sample rate is outside 8–192 kHz";
            return false;
        }
        const auto source_rate = out.sample_rate;
        out.mono = resample_to_detector_rate(out.mono, source_rate);
        out.sample_rate = k_default_sample_rate;
        if (out.mono.size() < k_fft_size) {
            error = "WAV is too short after resampling to 16 kHz";
            return false;
        }
        pm::audio::log::info("[sound-trigger] resampled WAV rate="
            + std::to_string(source_rate) + " to="
            + std::to_string(k_default_sample_rate)
            + " samples=" + std::to_string(out.mono.size()));
    }
    return true;
}

std::vector<FrameMetrics> inspect_pcm(const std::int16_t* pcm,
                                      std::size_t samples,
                                      std::uint32_t sample_rate)
{
    std::vector<FrameMetrics> result;
    if (!pcm || sample_rate < 8000 || samples < k_fft_size)
        return result;
    result.reserve((samples - k_fft_size) / k_hop_size + 1);
    Spectrum fft;
    SpectrumBins spectrum{};
    WhistleTracker ridge;
    const WhistleConfig band;
    float noise = estimate_noise_rms(pcm, samples, sample_rate);
    for (std::size_t offset = 0; offset <= samples - k_fft_size;
         offset += k_hop_size) {
        const auto time_ms = static_cast<std::uint64_t>(offset) * 1000 / sample_rate;
        auto m = basic_metrics(pcm + offset, noise, time_ms);
        const SpectrumBins* spec = nullptr;
        if (m.active) {
            fft.power(pcm + offset, spectrum);
            add_spectral_metrics(
                spectrum, sample_rate, band.min_hz, band.max_hz, m);
            spec = &spectrum;
        }
        if (!m.active)
            noise = 0.995f * noise + 0.005f * m.rms;
        m.noise_rms = noise;
        ridge.inspect(spec, m, sample_rate, time_ms, band, false);
        if (pm::audio::log::trace_enabled()) {
            pm::audio::log::trace("[sound-trigger] frame t_ms="
                + std::to_string(m.timestamp_ms)
                + " rms=" + std::to_string(m.rms)
                + " noise=" + std::to_string(m.noise_rms)
                + " peak_hz=" + std::to_string(m.dominant_hz)
                + " tracked_hz=" + std::to_string(m.tracked_peak_hz)
                + " concentration=" + std::to_string(m.concentration)
                + " flatness=" + std::to_string(m.flatness)
                + " band_ratio=" + std::to_string(m.band_ratio)
                + " prominence=" + std::to_string(m.prominence)
                + " state=" + ridge_state_name(m.ridge_state)
                + " whistle=" + (m.whistle ? "1" : "0"));
        }
        result.push_back(m);
    }
    FrameMetrics tail;
    tail.timestamp_ms = static_cast<std::uint64_t>(samples) * 1000 / sample_rate;
    ridge.inspect(nullptr, tail, sample_rate, tail.timestamp_ms, band, true);
    if (tail.ridge_reason != RidgeReason::none)
        result.push_back(tail);
    return result;
}

TemplateSequence extract_template(const std::int16_t* pcm,
                                  std::size_t samples,
                                  std::uint32_t sample_rate)
{
    return event_features(pcm, samples, sample_rate);
}

float dtw_distance(const TemplateSequence& a, const TemplateSequence& b,
                   float band_ratio)
{
    const std::size_t n = a.frames.size();
    const std::size_t m = b.frames.size();
    if (n == 0 || m == 0
        || n > k_max_template_frames || m > k_max_template_frames
        || !std::isfinite(band_ratio) || band_ratio < 0.0f
        || band_ratio > 1.0f)
        return k_inf;
    const std::size_t shorter = std::min(n, m);
    const std::size_t longer = std::max(n, m);
    if (longer > shorter * 3U)
        return k_inf;
    const std::size_t band = std::max<std::size_t>(
        std::max(n, m) > std::min(n, m) ? std::max(n, m) - std::min(n, m) : 0,
        static_cast<std::size_t>(std::ceil(std::max(n, m) * band_ratio)));
    thread_local std::vector<float> prev;
    thread_local std::vector<float> cur;
    prev.assign(m + 1, k_inf);
    cur.assign(m + 1, k_inf);
    prev[0] = 0.0f;
    for (std::size_t i = 1; i <= n; ++i) {
        std::fill(cur.begin(), cur.end(), k_inf);
        const std::size_t first = i > band ? i - band : 1;
        const std::size_t last = std::min(m, i + band);
        for (std::size_t j = first; j <= last; ++j) {
            const float cost = feature_distance(a.frames[i - 1], b.frames[j - 1]);
            cur[j] = cost + std::min({prev[j], cur[j - 1], prev[j - 1]});
        }
        prev.swap(cur);
    }
    return prev[m] / static_cast<float>(n + m);
}

TrainingResult train_profile(std::string trigger_id,
                             const std::vector<WavData>& positives,
                             const std::vector<WavData>& negatives)
{
    TrainingResult out;
    if (trigger_id.empty()) {
        out.error = "trigger id is required";
        return out;
    }
    if (positives.size() < 8) {
        out.error = "at least 8 positive takes are required";
        return out;
    }
    if (negatives.empty()) {
        out.error = "at least one ambient or hard-negative take is required";
        return out;
    }
    std::vector<TemplateSequence> templates;
    templates.reserve(positives.size());
    for (const auto& wav : positives) {
        auto t = extract_template(wav.mono.data(), wav.mono.size(), wav.sample_rate);
        if (t.frames.size() < k_min_template_frames
            || t.frames.size() > k_max_template_frames) {
            ++out.rejected;
            continue;
        }
        ++out.accepted;
        templates.push_back(std::move(t));
    }
    if (templates.size() < 8) {
        out.error = "fewer than 8 positive takes passed signal-quality checks";
        return out;
    }
    std::vector<float> positive_scores;
    for (std::size_t i = 0; i < templates.size(); ++i) {
        float best = k_inf;
        for (std::size_t j = 0; j < templates.size(); ++j) {
            if (i != j)
                best = std::min(best, dtw_distance(templates[i], templates[j]));
        }
        positive_scores.push_back(best);
    }
    if (!std::all_of(positive_scores.begin(), positive_scores.end(),
                     [](float value) { return std::isfinite(value); })) {
        out.error = "positive takes have incompatible durations";
        return out;
    }
    std::vector<float> negative_scores;
    for (const auto& wav : negatives) {
        const auto candidate =
            extract_template(wav.mono.data(), wav.mono.size(), wav.sample_rate);
        if (candidate.frames.empty())
            continue;
        float best = k_inf;
        for (const auto& t : templates)
            best = std::min(best, dtw_distance(candidate, t));
        negative_scores.push_back(best);
    }
    if (negative_scores.empty()) {
        out.error = "no ambient take contained an analyzable event";
        return out;
    }
    negative_scores.erase(
        std::remove_if(negative_scores.begin(), negative_scores.end(),
                       [](float value) { return !std::isfinite(value); }),
        negative_scores.end());
    if (negative_scores.empty()) {
        out.error = "negative takes have incompatible durations";
        return out;
    }
    out.positive_max = *std::max_element(positive_scores.begin(), positive_scores.end());
    out.negative_min = *std::min_element(negative_scores.begin(), negative_scores.end());
    if (out.negative_min <= out.positive_max * 1.05f) {
        out.error = "positive and negative validation scores overlap";
        return out;
    }
    out.profile.trigger_id = std::move(trigger_id);
    out.profile.threshold = (out.positive_max + out.negative_min) * 0.5f;
    // Unknown-sound rejection is calibrated by threshold. This margin is only
    // for ambiguity between two armed trigger profiles.
    out.profile.margin = std::max(0.01f, out.positive_max * 0.10f);
    out.profile.templates = std::move(templates);
    out.ok = true;
    pm::audio::log::info("[sound-trigger] trained id=" + out.profile.trigger_id
        + " calibration=leave-one-out"
        + " accepted=" + std::to_string(out.accepted)
        + " rejected=" + std::to_string(out.rejected)
        + " threshold=" + std::to_string(out.profile.threshold)
        + " inter_trigger_margin=" + std::to_string(out.profile.margin));
    return out;
}

bool save_profile(const Profile& profile, const std::filesystem::path& path,
                  std::string& error)
{
    if (!validate_profile(profile, error))
        return false;
    const std::uint64_t hash = feature_hash(profile.templates);
    const std::string feature_name = path.filename().string() + "."
        + hex_hash(hash) + ".features.bin";
    const auto feature_path = path.parent_path() / feature_name;
    const auto feature_temp = feature_path.string() + ".tmp";
    std::error_code ec;
    if (!path.parent_path().empty())
        std::filesystem::create_directories(path.parent_path(), ec);
    if (ec) {
        error = "cannot create profile directory: " + ec.message();
        return false;
    }
    {
        std::ofstream features(feature_temp, std::ios::binary | std::ios::trunc);
        const char magic[8] = {'P', 'M', 'S', 'T', 'F', '0', '2', '\0'};
        const std::uint32_t count =
            static_cast<std::uint32_t>(profile.templates.size());
        features.write(magic, sizeof(magic));
        write_u32le(features, count);
        for (const auto& t : profile.templates) {
            const std::uint32_t frames =
                static_cast<std::uint32_t>(t.frames.size());
            write_u32le(features, frames);
            for (const auto& frame : t.frames) {
                for (float value : frame)
                    write_f32le(features, value);
            }
        }
        if (!features) {
            error = "failed writing profile feature data";
            return false;
        }
    }
    std::filesystem::rename(feature_temp, feature_path, ec);
    if (ec && std::filesystem::exists(feature_path)) {
        std::filesystem::remove(feature_temp, ec);
        ec.clear();
    }
    if (ec) {
        error = "cannot atomically install feature data: " + ec.message();
        return false;
    }
    nlohmann::json j{
        {"schema", "pm.sound.profile/v1"},
        {"schema_version", profile.schema_version},
        {"feature_version", profile.feature_version},
        {"feature_encoding", "float32-le"},
        {"normalization", "log-band-zscore-v1"},
        {"trigger_id", profile.trigger_id},
        {"detector", profile.detector},
        {"sample_rate", profile.sample_rate},
        {"fft_size", profile.fft_size},
        {"hop_size", profile.hop_size},
        {"feature_bins", profile.feature_bins},
        {"threshold", profile.threshold},
        {"margin", profile.margin},
        {"whistle", whistle_json(profile.whistle)},
        {"feature_file", feature_name},
        {"feature_hash", hex_hash(hash)},
        {"template_count", profile.templates.size()},
    };
    const auto temp = path.string() + ".tmp";
    {
        std::ofstream out(temp, std::ios::binary | std::ios::trunc);
        if (!out) {
            error = "cannot create profile temporary file";
            return false;
        }
        out << j.dump(2) << '\n';
        if (!out) {
            error = "failed writing profile";
            return false;
        }
    }
    std::filesystem::rename(temp, path, ec);
    if (ec && std::filesystem::exists(path)) {
        const auto backup = path.string() + ".bak";
        std::error_code backup_error;
        std::filesystem::remove(backup, backup_error);
        backup_error.clear();
        std::filesystem::rename(path, backup, backup_error);
        if (backup_error) {
            error = "cannot preserve previous profile: " + backup_error.message();
            std::filesystem::remove(temp, backup_error);
            return false;
        }
        ec.clear();
        std::filesystem::rename(temp, path, ec);
        if (ec) {
            std::error_code rollback_error;
            std::filesystem::rename(backup, path, rollback_error);
            error = "cannot replace profile: " + ec.message();
            if (rollback_error)
                error += "; rollback failed: " + rollback_error.message();
            std::filesystem::remove(temp, rollback_error);
            return false;
        }
        std::filesystem::remove(backup, backup_error);
    }
    if (ec) {
        error = "cannot install profile: " + ec.message();
        std::filesystem::remove(temp, ec);
        return false;
    }
    return true;
}

bool load_profile(const std::filesystem::path& path, Profile& profile,
                  std::string& error)
{
    try {
        std::ifstream in(path, std::ios::binary);
        if (!in) {
            error = "cannot open sound-trigger profile";
            return false;
        }
        nlohmann::json j;
        in >> j;
        if (j.value("schema", std::string{}) != "pm.sound.profile/v1") {
            error = "unknown sound-trigger profile schema";
            return false;
        }
        Profile p;
        p.schema_version = j.value("schema_version", 0U);
        p.feature_version = j.value("feature_version", 0U);
        p.trigger_id = j.value("trigger_id", std::string{});
        p.detector = j.value("detector", std::string{});
        p.sample_rate = j.value("sample_rate", 0U);
        p.fft_size = j.value("fft_size", 0U);
        p.hop_size = j.value("hop_size", 0U);
        p.feature_bins = j.value("feature_bins", 0U);
        p.threshold = j.value("threshold", 0.0f);
        p.margin = j.value("margin", 0.0f);
        if (j.contains("whistle"))
            read_whistle(j["whistle"], p.whistle);
        if (p.schema_version != k_profile_schema_version
            || p.feature_version != k_feature_version
            || j.value("feature_encoding", std::string{}) != "float32-le"
            || j.value("normalization", std::string{})
                != "log-band-zscore-v1"
            || p.sample_rate != k_default_sample_rate
            || p.fft_size != k_fft_size
            || p.hop_size != k_hop_size
            || p.feature_bins != k_feature_bins) {
            error = "sound-trigger profile uses incompatible feature settings";
            return false;
        }
        const std::string feature_name =
            j.value("feature_file", std::string{});
        if (!feature_name.empty()) {
            if (std::filesystem::path(feature_name).filename() != feature_name) {
                error = "invalid sound-trigger feature filename";
                return false;
            }
            const auto feature_path = path.parent_path() / feature_name;
            std::error_code size_error;
            const auto feature_size =
                std::filesystem::file_size(feature_path, size_error);
            if (size_error || feature_size > 8U * 1024U * 1024U) {
                error = "sound-trigger feature data is missing or too large";
                return false;
            }
            std::ifstream features(feature_path, std::ios::binary);
            char magic[8]{};
            std::uint32_t count = 0;
            features.read(magic, sizeof(magic));
            if (!read_u32le(features, count)
                || std::memcmp(magic, "PMSTF02", 7) != 0
                || count > k_max_templates) {
                error = "invalid sound-trigger feature data";
                return false;
            }
            if (count != j.value("template_count", 0U)) {
                error = "sound-trigger template count mismatch";
                return false;
            }
            for (std::uint32_t i = 0; i < count; ++i) {
                std::uint32_t frames = 0;
                if (!read_u32le(features, frames)
                    || frames == 0 || frames > k_max_template_frames) {
                    error = "invalid sound-trigger template length";
                    return false;
                }
                TemplateSequence t;
                t.frames.assign(frames, FeatureFrame{});
                for (auto& frame : t.frames) {
                    for (float& value : frame) {
                        if (!read_f32le(features, value)) {
                            error = "truncated sound-trigger feature data";
                            return false;
                        }
                    }
                }
                if (!features) {
                    error = "truncated sound-trigger feature data";
                    return false;
                }
                p.templates.push_back(std::move(t));
            }
            const std::string expected = j.value("feature_hash", std::string{});
            if (expected.empty() || expected != hex_hash(feature_hash(p.templates))) {
                error = "sound-trigger feature hash mismatch";
                return false;
            }
        }
        if (!validate_profile(p, error))
            return false;
        profile = std::move(p);
        return true;
    } catch (const std::exception& e) {
        error = std::string("invalid sound-trigger profile: ") + e.what();
        return false;
    }
}

struct Engine::Impl {
    std::vector<Profile> profiles;
    std::vector<WhistleTracker> whistle_states;
    Spectrum fft;
    SpectrumBins spectrum{};
    std::vector<std::int16_t> pending;
    std::size_t pending_cursor = 0;
    EventBuilder events;
    bool has_template_profiles = false;
    bool has_whistle_profiles = false;
    std::uint32_t sample_rate = 0;
    std::uint64_t stream_start_ms = 0;
    std::uint64_t consumed_samples = 0;
    std::uint64_t last_input_end_ms = 0;
    float noise = 30.0f;
    std::vector<float> calibration_levels;
    bool calibrated = false;
    std::uint64_t event_start_ms = 0;

    void reset()
    {
        pending.clear();
        pending_cursor = 0;
        events = {};
        sample_rate = 0;
        stream_start_ms = 0;
        consumed_samples = 0;
        last_input_end_ms = 0;
        noise = 30.0f;
        calibration_levels.clear();
        calibrated = false;
        event_start_ms = 0;
        whistle_states.clear();
        whistle_states.resize(profiles.size());
    }

    void process_whistles(const FrameMetrics& base,
                          const SpectrumBins* frame_spectrum,
                          std::uint64_t frame_ms,
                          std::vector<Detection>& detections,
                          bool flush)
    {
        for (std::size_t i = 0; i < profiles.size(); ++i) {
            if (profiles[i].detector != "double-whistle")
                continue;
            whistle_states[i].observe(
                base, frame_spectrum, sample_rate, frame_ms,
                profiles[i].whistle, profiles[i].trigger_id, flush, detections);
        }
    }

    std::vector<Detection> classify(std::uint64_t end_ms)
    {
        std::vector<Detection> detections;
        if (events.sequence.frames.empty())
            return detections;
        const auto& candidate = events.sequence;
        float best = k_inf;
        float second = k_inf;
        const Profile* winner = nullptr;
        for (const auto& profile : profiles) {
            if (profile.detector != "template-dtw")
                continue;
            float score = k_inf;
            for (const auto& t : profile.templates)
                score = std::min(score, dtw_distance(candidate, t));
            if (pm::audio::log::debug_enabled()) {
                pm::audio::log::debug("[sound-trigger] candidate id="
                    + profile.trigger_id
                    + " distance=" + std::to_string(score)
                    + " threshold=" + std::to_string(profile.threshold));
            }
            if (score < best) {
                second = best;
                best = score;
                winner = &profile;
            } else if (score < second) {
                second = score;
            }
        }
        const bool beats_threshold = winner != nullptr
            && std::isfinite(best) && best <= winner->threshold;
        const bool ambiguous = winner != nullptr && std::isfinite(second)
            && second - best < winner->margin;
        if (beats_threshold && !ambiguous) {
            Detection d;
            d.operation_id = next_operation_id();
            d.trigger_id = winner->trigger_id;
            d.score_kind = "distance-threshold";
            d.score = winner->threshold > 0.0f
                ? std::max(0.0f, 1.0f - best / winner->threshold) : 0.0f;
            d.start_time_ms = event_start_ms;
            d.end_time_ms = end_ms;
            detections.push_back(std::move(d));
        }
        events.sequence.frames.clear();
        return detections;
    }
};

Engine::Engine() : impl_(std::make_unique<Impl>()) {}
Engine::~Engine() = default;
Engine::Engine(Engine&&) noexcept = default;
Engine& Engine::operator=(Engine&&) noexcept = default;

bool Engine::add_profile(Profile profile, std::string& error)
{
    if (!validate_profile(profile, error))
        return false;
    impl_->has_template_profiles =
        impl_->has_template_profiles || profile.detector == "template-dtw";
    impl_->has_whistle_profiles =
        impl_->has_whistle_profiles || profile.detector == "double-whistle";
    impl_->profiles.push_back(std::move(profile));
    impl_->whistle_states.emplace_back();
    return true;
}

void Engine::reset() noexcept
{
    impl_->reset();
}

std::vector<Detection> Engine::process(const std::int16_t* pcm,
                                       std::size_t samples,
                                       std::uint32_t sample_rate,
                                       std::uint64_t monotonic_time_ms)
{
    std::vector<Detection> out;
    if (!pcm || samples == 0)
        return out;
    if (sample_rate != k_default_sample_rate) {
        pm::audio::log::warn("[sound-trigger] rejected PCM rate=" + std::to_string(sample_rate)
            + " expected=" + std::to_string(k_default_sample_rate));
        reset();
        return out;
    }
    if (samples > static_cast<std::size_t>(sample_rate) * 10U) {
        pm::audio::log::warn("[sound-trigger] rejected oversized PCM block samples="
            + std::to_string(samples));
        reset();
        return out;
    }
    if (impl_->last_input_end_ms != 0) {
        const bool forward = monotonic_time_ms >= impl_->last_input_end_ms;
        const std::uint64_t distance = forward
            ? monotonic_time_ms - impl_->last_input_end_ms
            : impl_->last_input_end_ms - monotonic_time_ms;
        if ((!forward && distance > 20U) || (forward && distance > 50U)) {
            if (pm::audio::log::debug_enabled()) {
                pm::audio::log::debug("[sound-trigger] stream discontinuity direction="
                    + std::string(forward ? "forward" : "backward")
                    + " distance_ms=" + std::to_string(distance)
                    + " reset=1");
            }
            reset();
        }
    }
    if (impl_->sample_rate != 0 && impl_->sample_rate != sample_rate)
        reset();
    if (impl_->sample_rate == 0) {
        impl_->sample_rate = sample_rate;
        impl_->stream_start_ms = monotonic_time_ms;
    }
    impl_->last_input_end_ms = monotonic_time_ms
        + static_cast<std::uint64_t>(samples) * 1000 / sample_rate;
    if (impl_->pending_cursor > 0
        && (impl_->pending_cursor >= 4096
            || impl_->pending_cursor * 2 >= impl_->pending.size())) {
        impl_->pending.erase(
            impl_->pending.begin(),
            impl_->pending.begin()
                + static_cast<std::ptrdiff_t>(impl_->pending_cursor));
        impl_->pending_cursor = 0;
    }
    impl_->pending.insert(impl_->pending.end(), pcm, pcm + samples);
    while (impl_->pending.size() - impl_->pending_cursor >= k_fft_size) {
        const std::uint64_t frame_ms = impl_->stream_start_ms
            + impl_->consumed_samples * 1000 / sample_rate;
        const auto* frame = impl_->pending.data() + impl_->pending_cursor;
        auto m = basic_metrics(frame, impl_->noise, frame_ms);
        if (!impl_->calibrated) {
            impl_->calibration_levels.push_back(m.rms);
            if (impl_->calibration_levels.size() >= k_calibration_frames) {
                const std::size_t index = impl_->calibration_levels.size() / 5;
                std::nth_element(impl_->calibration_levels.begin(),
                    impl_->calibration_levels.begin() + index,
                    impl_->calibration_levels.end());
                impl_->noise = std::max(
                    20.0f, impl_->calibration_levels[index]);
                impl_->calibrated = true;
                if (pm::audio::log::debug_enabled()) {
                    pm::audio::log::debug("[sound-trigger] calibrated noise_rms="
                        + std::to_string(impl_->noise));
                }
            }
            impl_->pending_cursor += k_hop_size;
            impl_->consumed_samples += k_hop_size;
            continue;
        }
        const bool template_features = impl_->has_template_profiles
            && impl_->events.needs_features(m.active);
        const bool needs_spectrum =
            (m.active && impl_->has_whistle_profiles) || template_features;
        if (needs_spectrum)
            impl_->fft.power(frame, impl_->spectrum);
        if (impl_->has_whistle_profiles) {
            impl_->process_whistles(
                m, m.active && needs_spectrum ? &impl_->spectrum : nullptr,
                frame_ms, out, false);
        }
        if (!m.active)
            impl_->noise = 0.995f * impl_->noise + 0.005f * m.rms;
        if (pm::audio::log::trace_enabled()) {
            pm::audio::log::trace("[sound-trigger] engine t_ms="
                + std::to_string(frame_ms)
                + " active=" + (m.active ? "1" : "0")
                + " rms=" + std::to_string(m.rms)
                + " noise=" + std::to_string(impl_->noise));
        }
        if (impl_->has_template_profiles) {
            const bool starting = m.active && !impl_->events.open
                && !impl_->events.suppressed && !impl_->events.rejected;
            FeatureFrame feature{};
            const FeatureFrame* feature_ptr = nullptr;
            if (template_features) {
                feature = log_band_features(impl_->spectrum);
                feature_ptr = &feature;
            }
            if (starting) {
                impl_->event_start_ms = frame_ms;
                if (pm::audio::log::debug_enabled()) {
                    pm::audio::log::debug("[sound-trigger] candidate start t_ms="
                        + std::to_string(frame_ms));
                }
            }
            const auto status = impl_->events.observe(m.active, feature_ptr);
            if (status == EventBuilder::Status::rejected
                && pm::audio::log::debug_enabled()) {
                pm::audio::log::debug(
                    "[sound-trigger] candidate rejected reason=max-frames");
            }
            if (status == EventBuilder::Status::ready) {
                auto hits = impl_->classify(frame_ms);
                out.insert(out.end(), std::make_move_iterator(hits.begin()),
                           std::make_move_iterator(hits.end()));
            }
        }
        impl_->pending_cursor += k_hop_size;
        impl_->consumed_samples += k_hop_size;
    }
    return out;
}

std::vector<Detection> Engine::finish(std::uint64_t monotonic_time_ms)
{
    std::vector<Detection> out;
    impl_->process_whistles({}, nullptr, monotonic_time_ms, out, true);
    if (impl_->events.finish()) {
        auto hits = impl_->classify(monotonic_time_ms);
        out.insert(out.end(), std::make_move_iterator(hits.begin()),
                   std::make_move_iterator(hits.end()));
    }
    return out;
}

} // namespace pm::audio::trigger
