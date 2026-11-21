#pragma once

#include "pm/audio/capture.hpp"

#include <nlohmann/json.hpp>

#include <filesystem>
#include <string>

namespace pm::audio::aec3 {

enum class DelayMode { automatic, fixed };
enum class Scenario { silence, far_only, near_only, double_talk, transitions };

struct RunOptions {
    std::string input_device;
    std::string output_device;
    std::filesystem::path output_directory;
    std::filesystem::path render_source;
    std::filesystem::path capture_source;
    std::filesystem::path desktop_capture;
    std::filesystem::path near_source;
    std::filesystem::path session;
    std::string desktop_device;
    CaptureSource source = CaptureSource::mic;
    float mic_gain = 1.f;
    float desktop_gain = 1.f;
    Scenario scenario = Scenario::far_only;
    DelayMode delay_mode = DelayMode::automatic;
    int delay_ms = 0;
    int render_shift_ms = 0;
    int duration_seconds = 20;
    int playback_internal_rate_hz = 0;
    int playback_internal_channels = 0;
    int playback_period_frames = 0;
    float playback_volume = -1.f;
    int capture_internal_rate_hz = 0;
    int capture_internal_channels = 0;
    float capture_volume = -1.f;
    int render_pad_samples = 0;
    int capture_pad_samples = 0;
    int desktop_pad_samples = 0;
    bool aec_enabled = true;
    bool ns_enabled = false;
    bool agc_enabled = false;
    bool live = false;
};

struct RunResult {
    bool ok = false;
    std::string code;
    std::string error;
    int status = 200;
    std::filesystem::path session_directory;
    nlohmann::json summary = nlohmann::json::object();
};

struct BackendStatus {
    bool installed = false;
    bool active = false;
    std::string name = "none";
};

BackendStatus backend_status();
nlohmann::json devices_json();
RunResult run(const RunOptions& options);
RunResult replay(const RunOptions& options);
RunResult inspect(const std::filesystem::path& session);

const char* scenario_name(Scenario scenario);
bool scenario_from_name(const std::string& name, Scenario& scenario);

} // namespace pm::audio::aec3
