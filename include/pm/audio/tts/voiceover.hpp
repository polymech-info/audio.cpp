#pragma once

#include "pm/audio/constants.hpp"
#include "pm/audio/subs/subtitle_writer.hpp"

#include <nlohmann/json.hpp>

#include <cstdint>
#include <filesystem>
#include <functional>
#include <string>
#include <vector>

namespace pm::tts::voiceover {

namespace C = pm::audio::constants;

struct TtsCapabilities {
    bool speed_control        = false;
    bool character_alignment  = false;
    bool request_stitching    = false;
};

struct VoiceOverUtterance {
    std::int64_t t0_ms         = 0;
    std::int64_t nominal_t1_ms = 0;
    std::int64_t hard_t1_ms    = 0;
    std::string  text;
    std::vector<std::size_t> cue_indices;
};

struct SynthesizeRequest {
    std::string              text;
    std::string              previous_text;
    std::string              next_text;
    std::vector<std::string> previous_request_ids;
    std::vector<std::string> next_request_ids;
    float                    speed     = 1.0f;
    std::int64_t             seed      = -1;
    bool                     alignment = false;
};

struct VoiceOverTake {
    std::vector<std::int16_t> pcm; // canonical 48 kHz mono s16
    std::int64_t              duration_ms      = 0;
    std::string               request_id;
    float                     generation_speed = 1.0f;
    std::int64_t              seed             = -1;
    bool                      from_cache       = false;
};

struct Placement {
    std::int64_t t0_ms              = 0;
    std::int64_t t1_ms              = 0;
    float        stretch_ratio      = 1.0f;
    bool         consumed_gap       = false;
    bool         regenerated        = false;
    bool         truncated          = false;
    bool         unresolved         = false;
    bool         natural            = false;
    std::int64_t generated_ms       = 0;
    std::int64_t trimmed_ms         = 0;
    std::int64_t free_gap_ms        = 0;
    std::int64_t original_window_ms = 0;
};

struct OverflowReport {
    int            utterances  = 0;
    int            natural     = 0;
    int            used_gap    = 0;
    int            stretched   = 0;
    int            regenerated = 0;
    int            unresolved  = 0;
    nlohmann::json utterances_json = nlohmann::json::array();
};

enum class OverflowMode { Resolve, Truncate };

struct Options {
    OverflowMode overflow           = OverflowMode::Resolve;
    int          min_inter_gap_ms   = C::k_voiceover_min_inter_gap_ms;
    double       max_stretch        = C::k_voiceover_max_stretch;
    int          max_utterance_ms   = C::k_voiceover_max_utterance_ms;
    int          merge_score_limit  = C::k_voiceover_merge_score_limit;
    float        speed              = 1.0f;
    std::int64_t seed               = -1;
    bool         alignment          = false;
    std::filesystem::path cache_dir;
    std::string  provider;
    std::string  model;
    std::string  voice;
    std::string  cache_extra;
};

using SynthesizeFn = std::function<VoiceOverTake(const SynthesizeRequest&)>;

bool load_cues(const std::string& input,
               std::vector<pm::subs::Cue>& cues,
               std::string& err);

std::vector<VoiceOverUtterance> merge_utterances(
    const std::vector<pm::subs::Cue>& cues, const Options& opt);

bool decode_tts_bytes(const std::vector<std::uint8_t>& bytes,
                      const std::string& format_hint,
                      std::vector<std::int16_t>& pcm48_mono,
                      std::string& err);

bool render_timeline(const std::vector<pm::subs::Cue>& cues,
                     const Options& opt,
                     const TtsCapabilities& caps,
                     const SynthesizeFn& synth,
                     const std::filesystem::path& dst_wav,
                     OverflowReport& report,
                     std::string& err);

nlohmann::json report_to_json(const OverflowReport& r);

} // namespace pm::tts::voiceover
