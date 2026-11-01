#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <memory>
#include <string>
#include <vector>

namespace pm::audio::trigger {

inline constexpr std::uint32_t k_profile_schema_version = 1;
inline constexpr std::uint32_t k_feature_version = 1;
inline constexpr std::uint32_t k_default_sample_rate = 16000;
inline constexpr std::size_t k_fft_size = 512;
inline constexpr std::size_t k_hop_size = 160;
inline constexpr std::size_t k_spectrum_bins = k_fft_size / 2 + 1;
inline constexpr std::size_t k_feature_bins = 24;
inline constexpr std::size_t k_max_template_frames = 300;
inline constexpr std::size_t k_max_templates = 64;

using SpectrumBins = std::array<float, k_spectrum_bins>;
using FeatureFrame = std::array<float, k_feature_bins>;

enum class RidgeState : std::uint8_t {
    idle,
    acquiring,
    tracking,
    hole,
    released,
};

enum class RidgeReason : std::uint8_t {
    none,
    acquired,
    tracking,
    hole,
    rejected_sparse,
    rejected_duration,
    rejected_overlong,
    accepted,
    waiting_release,
};

inline const char* ridge_state_name(RidgeState state) noexcept
{
    switch (state) {
    case RidgeState::acquiring: return "acquiring";
    case RidgeState::tracking: return "tracking";
    case RidgeState::hole: return "hole";
    case RidgeState::released: return "released";
    case RidgeState::idle: break;
    }
    return "idle";
}

inline const char* ridge_reason_name(RidgeReason reason) noexcept
{
    switch (reason) {
    case RidgeReason::acquired: return "acquired";
    case RidgeReason::tracking: return "tracking";
    case RidgeReason::hole: return "hole";
    case RidgeReason::rejected_sparse: return "rejected-sparse";
    case RidgeReason::rejected_duration: return "rejected-duration";
    case RidgeReason::rejected_overlong: return "rejected-overlong";
    case RidgeReason::accepted: return "accepted";
    case RidgeReason::waiting_release: return "waiting-release";
    case RidgeReason::none: break;
    }
    return "none";
}

struct FrameMetrics {
    std::uint64_t timestamp_ms = 0;
    float rms = 0.0f;
    float noise_rms = 0.0f;
    float dominant_hz = 0.0f;
    float concentration = 0.0f;
    float flatness = 1.0f;
    // Energy inside the whistle band divided by energy below it.
    // Speech keeps a strong low band; a whistle does not.
    float band_ratio = 0.0f;
    // Power in the ridge core divided by the median power of the surrounding
    // bins, with the core itself excluded. See ridge_prominence().
    float prominence = 0.0f;
    float tracked_peak_hz = 0.0f;
    float predicted_peak_hz = 0.0f;
    float tracked_prominence = 0.0f;
    RidgeState ridge_state = RidgeState::idle;
    RidgeReason ridge_reason = RidgeReason::none;
    std::uint32_t hole_ms = 0;
    float good_hop_fraction = 0.0f;
    bool active = false;
    bool whistle = false;
};

struct Detection {
    std::string operation_id;
    std::string trigger_id;
    std::string score_kind = "distance-threshold";
    float score = 0.0f;
    std::uint64_t start_time_ms = 0;
    std::uint64_t end_time_ms = 0;
    std::uint32_t profile_version = k_profile_schema_version;
};

struct WhistleConfig {
    float min_hz = 1000.0f;
    float max_hz = 4000.0f;
    // Real whistles are narrow but not pure sines: about a third of the
    // 1–4 kHz band sits in the peak, and flatness stays below speech-like noise.
    float min_concentration = 0.28f;
    float max_flatness = 0.50f;
    float min_band_ratio = 2.0f;
    float max_pitch_jump_hz = 350.0f;
    std::uint32_t min_duration_ms = 100;
    std::uint32_t max_duration_ms = 700;
    std::uint32_t min_gap_ms = 150;
    std::uint32_t max_gap_ms = 900;
    std::uint32_t cooldown_ms = 2500;
};

struct TemplateSequence {
    std::vector<FeatureFrame> frames;
};

struct Profile {
    std::uint32_t schema_version = k_profile_schema_version;
    std::uint32_t feature_version = k_feature_version;
    std::string trigger_id;
    std::string detector = "template-dtw";
    std::uint32_t sample_rate = k_default_sample_rate;
    std::uint32_t fft_size = 512;
    std::uint32_t hop_size = 160;
    std::uint32_t feature_bins = 24;
    float threshold = 0.0f;
    float margin = 0.0f;
    WhistleConfig whistle;
    std::vector<TemplateSequence> templates;
};

struct TrainingResult {
    bool ok = false;
    std::string error;
    Profile profile;
    float positive_max = 0.0f;
    float negative_min = 0.0f;
    std::size_t accepted = 0;
    std::size_t rejected = 0;
};

struct WavData {
    std::uint32_t sample_rate = 0;
    std::vector<std::int16_t> mono;
};

// PCM16 mono or stereo. Stereo is averaged. Rates other than 16 kHz are
// low-pass resampled so the detector always sees k_default_sample_rate.
bool read_wav_mono16(const std::filesystem::path& path, WavData& out,
                     std::string& error);
bool save_profile(const Profile& profile, const std::filesystem::path& path,
                  std::string& error);
bool load_profile(const std::filesystem::path& path, Profile& profile,
                  std::string& error);

std::vector<FrameMetrics> inspect_pcm(const std::int16_t* pcm,
                                      std::size_t samples,
                                      std::uint32_t sample_rate);
TemplateSequence extract_template(const std::int16_t* pcm,
                                  std::size_t samples,
                                  std::uint32_t sample_rate);
float dtw_distance(const TemplateSequence& a, const TemplateSequence& b,
                   float band_ratio = 0.15f);

TrainingResult train_profile(std::string trigger_id,
                             const std::vector<WavData>& positives,
                             const std::vector<WavData>& negatives);

class Engine {
public:
    Engine();
    ~Engine();
    Engine(Engine&&) noexcept;
    Engine& operator=(Engine&&) noexcept;
    Engine(const Engine&) = delete;
    Engine& operator=(const Engine&) = delete;

    bool add_profile(Profile profile, std::string& error);
    void reset() noexcept;
    std::vector<Detection> process(const std::int16_t* pcm,
                                   std::size_t samples,
                                   std::uint32_t sample_rate,
                                   std::uint64_t monotonic_time_ms);
    std::vector<Detection> finish(std::uint64_t monotonic_time_ms);

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

} // namespace pm::audio::trigger
