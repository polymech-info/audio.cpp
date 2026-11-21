#include "pm/audio/aec3_lab.hpp"

#include "aec3_artifacts.hpp"
#include "aec3_framer.hpp"
#include "aec3_metrics.hpp"

#include "pm/audio/devices.hpp"
#include "pm/audio/echo.hpp"
#include "pm/audio/stt/wav.hpp"
#include "pm/audio/text.hpp"

#if defined(_MSC_VER)
#  pragma warning(push, 0)
#endif
#include "miniaudio.h"
#if defined(_MSC_VER)
#  pragma warning(pop)
#endif

#include <algorithm>
#include <chrono>
#include <cmath>
#include <condition_variable>
#include <cstring>
#include <fstream>
#include <mutex>
#include <thread>
#include <vector>

namespace pm::audio::aec3 {
namespace {

constexpr int kRate = 16000;
constexpr std::size_t kFrame = 160;

RunResult fail(const std::string& code, const std::string& error, int status)
{
    RunResult result;
    result.code = code;
    result.error = error;
    result.status = status;
    return result;
}

std::int16_t clamp_s16(float value)
{
    value = std::clamp(value, -32768.f, 32767.f);
    return static_cast<std::int16_t>(std::lround(value));
}

std::int16_t scaled(std::int16_t sample, float gain)
{
    if (gain == 1.f)
        return sample;
    return clamp_s16(static_cast<float>(sample) * gain);
}

std::vector<std::int16_t> mix_legs(const std::vector<std::int16_t>& mic,
                                   const std::vector<std::int16_t>& desktop,
                                   float mic_gain,
                                   float desktop_gain,
                                   bool mic_clock)
{
    const std::size_t count = mic_clock ? mic.size() : std::max(mic.size(), desktop.size());
    std::vector<std::int16_t> mixed(count);
    for (std::size_t i = 0; i < count; ++i) {
        const float left = i < mic.size() ? static_cast<float>(mic[i]) * mic_gain : 0.f;
        const float right = i < desktop.size() ? static_cast<float>(desktop[i]) * desktop_gain : 0.f;
        mixed[i] = clamp_s16(left + right);
    }
    return mixed;
}

bool load_s16(const std::filesystem::path& path, std::vector<std::int16_t>& out, std::string& error)
{
    try {
        const pm::stt::WavPcm wav = pm::stt::read_wav(path_to_utf8(path));
        const std::vector<float> mono = wav.sample_rate == static_cast<std::uint32_t>(kRate)
            ? wav.mono
            : pm::stt::resample_16k(wav);
        out.resize(mono.size());
        for (std::size_t i = 0; i < mono.size(); ++i)
            out[i] = clamp_s16(mono[i] * 32768.f);
        return true;
    } catch (const std::exception& ex) {
        error = ex.what();
        return false;
    }
}

std::vector<std::int16_t> shift_render(const std::vector<std::int16_t>& render, int shift_ms)
{
    if (shift_ms == 0)
        return render;
    const int samples = shift_ms * kRate / 1000;
    std::vector<std::int16_t> shifted(render.size(), 0);
    for (std::size_t i = 0; i < render.size(); ++i) {
        const std::ptrdiff_t source = static_cast<std::ptrdiff_t>(i) - samples;
        if (source >= 0 && static_cast<std::size_t>(source) < render.size())
            shifted[i] = render[static_cast<std::size_t>(source)];
    }
    return shifted;
}

nlohmann::json level_json(const Level& level)
{
    return {
        {"rms", level.rms},
        {"peak", level.peak},
        {"clips", level.clips},
        {"samples", level.samples},
    };
}

nlohmann::json statistics_json(const ApmStatistics& stats, bool present)
{
    nlohmann::json body = {{"present", present}};
    if (!present)
        return body;
    if (stats.has_echo_return_loss)
        body["echo_return_loss"] = stats.echo_return_loss;
    if (stats.has_echo_return_loss_enhancement)
        body["echo_return_loss_enhancement"] = stats.echo_return_loss_enhancement;
    if (stats.has_divergent_filter_fraction)
        body["divergent_filter_fraction"] = stats.divergent_filter_fraction;
    if (stats.has_delay_median_ms)
        body["delay_median_ms"] = stats.delay_median_ms;
    if (stats.has_delay_standard_deviation_ms)
        body["delay_standard_deviation_ms"] = stats.delay_standard_deviation_ms;
    if (stats.has_residual_echo_likelihood)
        body["residual_echo_likelihood"] = stats.residual_echo_likelihood;
    if (stats.has_residual_echo_likelihood_recent_max)
        body["residual_echo_likelihood_recent_max"] = stats.residual_echo_likelihood_recent_max;
    if (stats.has_delay_ms)
        body["delay_ms"] = stats.delay_ms;
    return body;
}

nlohmann::json apm_timeline_json(const std::vector<ApmStatistics>& timeline)
{
    nlohmann::json rows = nlohmann::json::array();
    for (std::size_t i = 0; i < timeline.size(); ++i) {
        nlohmann::json row = statistics_json(timeline[i], true);
        row["t_ms"] = static_cast<int>((i + 1) * 1000);
        rows.push_back(std::move(row));
    }
    return rows;
}

struct Processed {
    std::vector<std::int16_t> samples;
    std::uint64_t frames = 0;
    std::size_t capture_remainder = 0;
    std::size_t render_remainder = 0;
    double process_us = 0;
    ApmStatistics statistics{};
    std::vector<ApmStatistics> timeline;
    bool statistics_present = false;
    bool aec_active = false;
    std::string backend = "none";
};

Processed process_streams(const std::vector<std::int16_t>& render,
                          const std::vector<std::int16_t>& capture,
                          const RunOptions& options,
                          std::string& error)
{
    Processed processed;
    std::unique_ptr<EchoCancellationProcessor> aec;
    if (options.aec_enabled) {
        EchoCancellationProcessor::Config cfg;
        cfg.sample_rate_hz = kRate;
        cfg.channels = 1;
        cfg.agc = options.agc_enabled;
        cfg.ns = options.ns_enabled;
        aec = EchoCancellationProcessor::create(cfg);
        if (!aec || !aec->active()) {
            error = "WebRTC AEC backend is not installed";
            return processed;
        }
        processed.aec_active = true;
        processed.backend = aec->backend_name();
        if (options.delay_mode == DelayMode::fixed)
            aec->set_stream_delay_ms(options.delay_ms);
    }

    FrameBuffer render_frames(kFrame);
    FrameBuffer capture_frames(kFrame);
    render_frames.push(render.data(), render.size());
    capture_frames.push(capture.data(), capture.size());
    std::int16_t render_frame[kFrame];
    std::int16_t capture_frame[kFrame];
    const auto started = std::chrono::steady_clock::now();
    while (capture_frames.pop(capture_frame)) {
        if (aec && render_frames.pop(render_frame))
            aec->process_render_reference(render_frame, kFrame);
        if (aec)
            aec->process_capture(capture_frame, kFrame);
        processed.samples.insert(processed.samples.end(), capture_frame, capture_frame + kFrame);
        ++processed.frames;
        if (aec) {
            ApmStatistics sample;
            if (aec->statistics(sample)) {
                processed.statistics = sample;
                processed.statistics_present = true;
                if (processed.frames % 100 == 0)
                    processed.timeline.push_back(sample);
            }
        }
    }
    while (aec && render_frames.pop(render_frame))
        aec->process_render_reference(render_frame, kFrame);
    processed.capture_remainder = capture_frames.remainder();
    processed.render_remainder = render_frames.remainder();
    processed.process_us = std::chrono::duration<double, std::micro>(
        std::chrono::steady_clock::now() - started).count();
    if (aec && aec->statistics(processed.statistics))
        processed.statistics_present = true;
    return processed;
}

nlohmann::json metrics_for(const std::vector<std::int16_t>& render,
                           const std::vector<std::int16_t>& capture,
                           const Processed& processed)
{
    const Level render_level = measure_level(render.data(), render.size());
    const Level raw_level = measure_level(capture.data(), capture.size());
    const Level out_level = measure_level(processed.samples.data(), processed.samples.size());
    const LagEstimate raw_lag = estimate_lag(render.data(), render.size(), capture.data(), capture.size(), kRate / 2, 10);
    const LagEstimate out_lag = estimate_lag(render.data(), render.size(), processed.samples.data(), processed.samples.size(), kRate / 2, 10);
    return {
        {"schema", "pm.audio.aec3.metrics/v1"},
        {"sample_rate_hz", kRate},
        {"frame_samples", kFrame},
        {"frames", processed.frames},
        {"capture_remainder_samples", processed.capture_remainder},
        {"render_remainder_samples", processed.render_remainder},
        {"process_us", processed.process_us},
        {"process_us_per_frame", processed.frames ? processed.process_us / static_cast<double>(processed.frames) : 0},
        {"aec_active", processed.aec_active},
        {"backend", processed.backend},
        {"render", level_json(render_level)},
        {"capture_raw", level_json(raw_level)},
        {"capture_processed", level_json(out_level)},
        {"render_capture_lag_samples", raw_lag.valid ? nlohmann::json(raw_lag.lag_samples) : nlohmann::json(nullptr)},
        {"render_capture_correlation", raw_lag.valid ? nlohmann::json(raw_lag.correlation) : nlohmann::json(nullptr)},
        {"render_capture_lag_reliable", raw_lag.reliable},
        {"render_processed_lag_samples", out_lag.valid ? nlohmann::json(out_lag.lag_samples) : nlohmann::json(nullptr)},
        {"render_processed_correlation", out_lag.valid ? nlohmann::json(out_lag.correlation) : nlohmann::json(nullptr)},
        {"render_processed_lag_reliable", out_lag.reliable},
        {"apm", statistics_json(processed.statistics, processed.statistics_present)},
        {"apm_hz", apm_timeline_json(processed.timeline)},
    };
}

bool write_session(const std::filesystem::path& dir,
                   const RunOptions& options,
                   const std::vector<std::int16_t>& render,
                   const std::vector<std::int16_t>& capture,
                   const std::vector<std::int16_t>& mic,
                   const std::vector<std::int16_t>& desktop,
                   const Processed& processed,
                   const nlohmann::json& metrics,
                   std::string& error)
{
    std::error_code ec;
    std::filesystem::create_directories(dir, ec);
    if (ec) {
        error = "cannot create session directory";
        return false;
    }
    const auto render_path = dir / "render-reference.wav";
    const auto raw_path = dir / "capture-raw.wav";
    const auto processed_path = dir / "capture-processed.wav";
    const std::int16_t empty = 0;
    const std::int16_t* render_pcm = render.empty() ? &empty : render.data();
    const std::int16_t* raw_pcm = capture.empty() ? &empty : capture.data();
    const std::int16_t* processed_pcm = processed.samples.empty() ? &empty : processed.samples.data();
    if (!pm::stt::write_pcm_wav(path_to_utf8(render_path), render_pcm, render.size(), kRate) ||
        !pm::stt::write_pcm_wav(path_to_utf8(raw_path), raw_pcm, capture.size(), kRate) ||
        !pm::stt::write_pcm_wav(path_to_utf8(processed_path), processed_pcm, processed.samples.size(), kRate)) {
        error = "cannot write session wav";
        return false;
    }
    if (!mic.empty() && !pm::stt::write_pcm_wav(path_to_utf8(dir / "capture-mic.wav"), mic.data(), mic.size(), kRate)) {
        error = "cannot write microphone wav";
        return false;
    }
    if (!desktop.empty() && !pm::stt::write_pcm_wav(path_to_utf8(dir / "capture-desktop.wav"), desktop.data(), desktop.size(), kRate)) {
        error = "cannot write desktop wav";
        return false;
    }
    if (!options.near_source.empty()) {
        std::filesystem::copy_file(options.near_source, dir / "near-end-reference.wav",
                                   std::filesystem::copy_options::overwrite_existing, ec);
    }
    nlohmann::json manifest = {
        {"schema", "pm.audio.aec3.manifest/v1"},
        {"complete", false},
        {"created_utc", utc_stamp()},
        {"scenario", scenario_name(options.scenario)},
        {"source", capture_source_name(options.source)},
        {"live", options.live},
        {"sample_rate_hz", kRate},
        {"aec_enabled", options.aec_enabled},
        {"aec_active", processed.aec_active},
        {"backend", processed.backend},
        {"ns_enabled", options.ns_enabled},
        {"agc_enabled", options.agc_enabled},
        {"delay_mode", options.delay_mode == DelayMode::fixed ? "fixed" : "automatic"},
        {"delay_ms", options.delay_ms},
        {"render_shift_ms", options.render_shift_ms},
        {"mic_gain", options.mic_gain},
        {"desktop_gain", options.desktop_gain},
        {"input_device", options.input_device},
        {"output_device", options.output_device},
        {"desktop_device", options.desktop_device},
        {"playback_internal_rate_hz", options.playback_internal_rate_hz},
        {"playback_internal_channels", options.playback_internal_channels},
        {"playback_period_frames", options.playback_period_frames},
        {"playback_volume", options.playback_volume},
        {"capture_internal_rate_hz", options.capture_internal_rate_hz},
        {"capture_internal_channels", options.capture_internal_channels},
        {"capture_volume", options.capture_volume},
        {"render_pad_samples", options.render_pad_samples},
        {"capture_pad_samples", options.capture_pad_samples},
        {"desktop_pad_samples", options.desktop_pad_samples},
        {"render_fnv1a64", hash_file(render_path)},
        {"capture_fnv1a64", hash_file(raw_path)},
    };
    const auto events = dir / "events.jsonl";
    append_jsonl(events, {{"schema", "pm.audio.aec3.event/v1"}, {"event", "start"}, {"scenario", scenario_name(options.scenario)}});
    append_jsonl(events, {{"schema", "pm.audio.aec3.event/v1"}, {"event", "done"}, {"frames", processed.frames}});
    if (!write_json_file(dir / "metrics.json", metrics)) {
        error = "cannot write metrics";
        return false;
    }
    manifest["complete"] = true;
    if (!write_json_file(dir / "manifest.json", manifest)) {
        error = "cannot write manifest";
        return false;
    }
    return true;
}

RunResult finish_offline(const RunOptions& options,
                         std::vector<std::int16_t> render,
                         std::vector<std::int16_t> mic,
                         std::vector<std::int16_t> desktop)
{
    const bool want_mic = options.source != CaptureSource::desktop;
    const bool want_desktop = options.source != CaptureSource::mic;
    if (want_mic && mic.empty())
        return fail("EINVAL", "microphone capture is empty", 400);
    if (want_desktop && desktop.empty())
        return fail("EINVAL", "desktop capture is empty", 400);
    if (!want_mic)
        mic.clear();
    if (!want_desktop)
        desktop.clear();

    const std::size_t clock = options.source == CaptureSource::desktop ? desktop.size() : mic.size();
    render = shift_render(render, options.render_shift_ms);
    if (options.scenario == Scenario::silence)
        render.assign(std::max(render.size(), clock), 0);
    if (options.scenario == Scenario::near_only)
        render.assign(clock, 0);
    if (options.scenario == Scenario::transitions) {
        const std::size_t chunk = static_cast<std::size_t>(kRate) / 2;
        for (std::size_t i = 0; i < render.size(); i += chunk * 2) {
            const std::size_t begin = std::min(render.size(), i + chunk);
            const std::size_t end = std::min(render.size(), begin + chunk);
            std::fill(render.begin() + static_cast<std::ptrdiff_t>(begin),
                      render.begin() + static_cast<std::ptrdiff_t>(end), 0);
        }
    }

    std::string error;
    Processed mic_processed;
    Processed desktop_processed;
    if (want_mic) {
        mic_processed = process_streams(render, mic, options, error);
        if (!error.empty())
            return fail("ENOTSUP", error, 501);
    }
    if (want_desktop) {
        RunOptions desktop_options = options;
        if (options.source == CaptureSource::mix)
            desktop_options.delay_mode = DelayMode::automatic;
        desktop_processed = process_streams(render, desktop, desktop_options, error);
        if (!error.empty())
            return fail("ENOTSUP", error, 501);
    }

    const std::vector<std::int16_t> raw = options.source == CaptureSource::mic
        ? mic
        : options.source == CaptureSource::desktop
            ? desktop
            : mix_legs(mic, desktop, options.mic_gain, options.desktop_gain, true);
    Processed processed = options.source == CaptureSource::desktop ? desktop_processed : mic_processed;
    if (options.source == CaptureSource::mix) {
        processed.samples = mix_legs(mic_processed.samples, desktop_processed.samples,
                                     options.mic_gain, options.desktop_gain, true);
        processed.frames = processed.samples.size() / kFrame;
        processed.capture_remainder = mic_processed.capture_remainder;
    } else if (options.source == CaptureSource::mic && options.mic_gain != 1.f) {
        for (std::int16_t& sample : processed.samples)
            sample = scaled(sample, options.mic_gain);
    } else if (options.source == CaptureSource::desktop && options.desktop_gain != 1.f) {
        for (std::int16_t& sample : processed.samples)
            sample = scaled(sample, options.desktop_gain);
    }

    nlohmann::json metrics = metrics_for(render, raw, processed);
    if (options.source == CaptureSource::mix) {
        metrics["microphone_apm"] = statistics_json(mic_processed.statistics, mic_processed.statistics_present);
        metrics["microphone_apm_hz"] = apm_timeline_json(mic_processed.timeline);
        metrics["desktop_apm"] = statistics_json(desktop_processed.statistics, desktop_processed.statistics_present);
        metrics["desktop_apm_hz"] = apm_timeline_json(desktop_processed.timeline);
    }
    std::filesystem::path dir = options.output_directory;
    if (dir.empty())
        dir = std::filesystem::temp_directory_path() / ("aec3-" + utc_stamp());
    if (!write_session(dir, options, render, raw, mic, desktop, processed, metrics, error))
        return fail("EIO", error, 500);
    RunResult result;
    result.ok = true;
    result.session_directory = dir;
    result.summary = {
        {"schema", "pm.audio.aec3.run/v1"},
        {"ok", true},
        {"session", path_to_utf8(dir)},
        {"source", capture_source_name(options.source)},
        {"aec_active", processed.aec_active},
        {"backend", processed.backend},
        {"frames", processed.frames},
        {"capture_remainder_samples", processed.capture_remainder},
        {"metrics", metrics},
    };
    return result;
}

std::vector<std::int16_t> builtin_render(int seconds)
{
    const std::size_t total = static_cast<std::size_t>(kRate) * static_cast<std::size_t>(std::max(1, seconds));
    std::vector<std::int16_t> pcm(total);
    constexpr double f0 = 200.0;
    constexpr double f1 = 4000.0;
    const double log_ratio = std::log(f1 / f0);
    for (std::size_t i = 0; i < total; ++i) {
        const double t = static_cast<double>(i % static_cast<std::size_t>(kRate)) / static_cast<double>(kRate);
        const double phase = 2.0 * 3.141592653589793 * f0 * (std::pow(f1 / f0, t) - 1.0) / log_ratio;
        pcm[i] = static_cast<std::int16_t>(std::lround(std::sin(phase) * 0.35 * 32767.0));
    }
    return pcm;
}

bool find_playback_id(const std::string& name, ma_device_id& id)
{
    if (name.empty())
        return false;
    ma_context ctx{};
    if (ma_context_init(nullptr, 0, nullptr, &ctx) != MA_SUCCESS)
        return false;
    ma_device_info* playback = nullptr;
    ma_device_info* capture = nullptr;
    ma_uint32 playback_count = 0;
    ma_uint32 capture_count = 0;
    bool found = false;
    if (ma_context_get_devices(&ctx, &playback, &playback_count, &capture, &capture_count) == MA_SUCCESS) {
        const std::string needle = lower_ascii(name);
        for (ma_uint32 i = 0; i < playback_count; ++i) {
            if (lower_ascii(playback[i].name).find(needle) != std::string::npos) {
                id = playback[i].id;
                found = true;
                break;
            }
        }
    }
    ma_context_uninit(&ctx);
    return found;
}

bool find_capture_id(const std::string& name, ma_device_id& id)
{
    if (name.empty())
        return false;
    ma_context ctx{};
    if (ma_context_init(nullptr, 0, nullptr, &ctx) != MA_SUCCESS)
        return false;
    ma_device_info* playback = nullptr;
    ma_device_info* capture = nullptr;
    ma_uint32 playback_count = 0;
    ma_uint32 capture_count = 0;
    bool found = false;
    if (ma_context_get_devices(&ctx, &playback, &playback_count, &capture, &capture_count) == MA_SUCCESS) {
        const std::string needle = lower_ascii(name);
        for (ma_uint32 i = 0; i < capture_count; ++i) {
            if (lower_ascii(capture[i].name).find(needle) != std::string::npos) {
                id = capture[i].id;
                found = true;
                break;
            }
        }
    }
    ma_context_uninit(&ctx);
    return found;
}

struct StreamClock {
    std::chrono::steady_clock::time_point first{};
    bool set = false;
};

struct LiveState {
    std::mutex mu;
    std::condition_variable cv;
    std::vector<std::int16_t> render;
    std::vector<std::int16_t> capture;
    std::vector<std::int16_t> desktop;
    std::vector<std::int16_t> playback;
    StreamClock render_clock;
    StreamClock capture_clock;
    StreamClock desktop_clock;
    std::size_t playback_cursor = 0;
    bool playback_done = false;
    bool stop = false;
};

void stamp(StreamClock& clock, std::chrono::steady_clock::time_point now)
{
    if (!clock.set) {
        clock.first = now;
        clock.set = true;
    }
}

int lead_samples(const StreamClock& clock, std::chrono::steady_clock::time_point origin)
{
    if (!clock.set)
        return 0;
    const double seconds = std::chrono::duration<double>(clock.first - origin).count();
    if (seconds <= 0)
        return 0;
    return static_cast<int>(std::llround(seconds * kRate));
}

void pad_front(std::vector<std::int16_t>& pcm, int samples)
{
    if (samples > 0)
        pcm.insert(pcm.begin(), static_cast<std::size_t>(samples), 0);
}

void playback_cb(ma_device* device, void* output, const void*, ma_uint32 frame_count)
{
    auto* live = static_cast<LiveState*>(device->pUserData);
    const auto now = std::chrono::steady_clock::now();
    auto* dst = static_cast<std::int16_t*>(output);
    std::vector<std::int16_t> played;
    {
        std::lock_guard<std::mutex> lock(live->mu);
        stamp(live->render_clock, now);
        const std::size_t available = live->playback.size() > live->playback_cursor
            ? live->playback.size() - live->playback_cursor : 0;
        const std::size_t n = std::min(available, static_cast<std::size_t>(frame_count));
        if (n > 0) {
            std::memcpy(dst, live->playback.data() + live->playback_cursor, n * sizeof(std::int16_t));
            played.assign(live->playback.data() + live->playback_cursor, live->playback.data() + live->playback_cursor + n);
            live->playback_cursor += n;
        }
        if (n < frame_count)
            std::memset(dst + n, 0, (static_cast<std::size_t>(frame_count) - n) * sizeof(std::int16_t));
        if (live->playback_cursor >= live->playback.size())
            live->playback_done = true;
        live->render.insert(live->render.end(), played.begin(), played.end());
    }
    live->cv.notify_one();
}

void capture_cb(ma_device* device, void*, const void* input, ma_uint32 frame_count)
{
    auto* live = static_cast<LiveState*>(device->pUserData);
    if (!input || frame_count == 0)
        return;
    const auto now = std::chrono::steady_clock::now();
    const auto* src = static_cast<const std::int16_t*>(input);
    {
        std::lock_guard<std::mutex> lock(live->mu);
        stamp(live->capture_clock, now);
        live->capture.insert(live->capture.end(), src, src + frame_count);
    }
    live->cv.notify_one();
}

RunResult run_live(const RunOptions& options)
{
    if (options.aec_enabled && !backend_status().active)
        return fail("ENOTSUP", "WebRTC AEC backend is not installed", 501);
    LiveState live;
    if (!options.render_source.empty()) {
        std::string error;
        if (!load_s16(options.render_source, live.playback, error))
            return fail("EIO", error, 400);
    } else if (options.scenario != Scenario::silence && options.scenario != Scenario::near_only) {
        live.playback = builtin_render(options.duration_seconds);
    }
    if (live.playback.empty())
        live.playback.assign(static_cast<std::size_t>(kRate) * static_cast<std::size_t>(std::max(1, options.duration_seconds)), 0);

    ma_device_id playback_id{};
    ma_device_id capture_id{};
    const bool named_out = find_playback_id(options.output_device, playback_id);
    const bool named_in = find_capture_id(options.input_device, capture_id);
    const bool want_mic = options.source != CaptureSource::desktop;
    const bool want_desktop = options.source != CaptureSource::mic;
    if (!options.output_device.empty() && !named_out)
        return fail("ENODEV", "no playback device matches the requested name", 404);
    if (want_mic && !options.input_device.empty() && !named_in)
        return fail("ENODEV", "no capture device matches the requested name", 404);

    ma_device playback{};
    ma_device capture{};
    bool playback_open = false;
    bool capture_open = false;
    DesktopAudioCapture loopback;
    bool loopback_open = false;

    ma_device_config playback_cfg = ma_device_config_init(ma_device_type_playback);
    playback_cfg.playback.format = ma_format_s16;
    playback_cfg.playback.channels = 1;
    playback_cfg.sampleRate = kRate;
    playback_cfg.dataCallback = playback_cb;
    playback_cfg.pUserData = &live;
    if (named_out)
        playback_cfg.playback.pDeviceID = &playback_id;
    if (ma_device_init(nullptr, &playback_cfg, &playback) != MA_SUCCESS)
        return fail("ENODEV", "playback device failed to open", 500);
    playback_open = true;

    if (want_mic) {
        ma_device_config capture_cfg = ma_device_config_init(ma_device_type_capture);
        capture_cfg.capture.format = ma_format_s16;
        capture_cfg.capture.channels = 1;
        capture_cfg.sampleRate = kRate;
        capture_cfg.dataCallback = capture_cb;
        capture_cfg.pUserData = &live;
        if (named_in)
            capture_cfg.capture.pDeviceID = &capture_id;
        if (ma_device_init(nullptr, &capture_cfg, &capture) != MA_SUCCESS) {
            ma_device_uninit(&playback);
            return fail("ENODEV", "capture device failed to open", 500);
        }
        capture_open = true;
    }
    if (want_desktop) {
        std::string loop_error;
        const std::string& loop_name = options.desktop_device.empty() ? options.output_device : options.desktop_device;
        if (!loopback.start([&live](const std::int16_t* pcm, std::size_t count) {
                if (!pcm || count == 0)
                    return;
                const auto now = std::chrono::steady_clock::now();
                {
                    std::lock_guard<std::mutex> lock(live.mu);
                    stamp(live.desktop_clock, now);
                    live.desktop.insert(live.desktop.end(), pcm, pcm + count);
                }
                live.cv.notify_one();
            }, loop_name, loop_error, static_cast<std::uint32_t>(kRate), 1u)) {
            if (capture_open)
                ma_device_uninit(&capture);
            if (playback_open)
                ma_device_uninit(&playback);
            return fail("ENODEV", loop_error.empty() ? "desktop loopback failed to open" : loop_error, 500);
        }
        loopback_open = true;
    }
    ma_device_set_master_volume(&playback, 1.f);
    if (capture_open)
        ma_device_set_master_volume(&capture, 1.f);
    if ((capture_open && ma_device_start(&capture) != MA_SUCCESS) || ma_device_start(&playback) != MA_SUCCESS) {
        if (loopback_open)
            loopback.stop();
        if (capture_open)
            ma_device_uninit(&capture);
        ma_device_uninit(&playback);
        return fail("ENODEV", "audio device failed to start", 500);
    }

    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(std::max(1, options.duration_seconds));
    {
        std::unique_lock<std::mutex> lock(live.mu);
        while (!live.stop && std::chrono::steady_clock::now() < deadline) {
            live.cv.wait_for(lock, std::chrono::milliseconds(50));
            const std::vector<std::int16_t>& clock = want_mic ? live.capture : live.desktop;
            if (live.playback_done && clock.size() >= live.render.size() + static_cast<std::size_t>(kRate) / 5)
                break;
        }
    }
    std::string opened_output = playback_open ? playback.playback.name : "";
    std::string opened_input = capture_open ? capture.capture.name : "";
    std::string opened_desktop = loopback_open ? loopback.opened_device_name() : "";
    const int playback_rate = playback_open ? static_cast<int>(playback.playback.internalSampleRate) : 0;
    const int playback_channels = playback_open ? static_cast<int>(playback.playback.internalChannels) : 0;
    const int playback_period = playback_open ? static_cast<int>(playback.playback.internalPeriodSizeInFrames) : 0;
    const int capture_rate = capture_open ? static_cast<int>(capture.capture.internalSampleRate) : 0;
    const int capture_channels = capture_open ? static_cast<int>(capture.capture.internalChannels) : 0;
    float playback_volume = -1.f;
    float capture_volume = -1.f;
    if (playback_open)
        ma_device_get_master_volume(&playback, &playback_volume);
    if (capture_open)
        ma_device_get_master_volume(&capture, &capture_volume);
    ma_device_stop(&playback);
    if (capture_open)
        ma_device_stop(&capture);
    if (loopback_open)
        loopback.stop();
    ma_device_uninit(&playback);
    if (capture_open)
        ma_device_uninit(&capture);

    std::vector<std::int16_t> render;
    std::vector<std::int16_t> captured;
    std::vector<std::int16_t> desktop_captured;
    {
        std::lock_guard<std::mutex> lock(live.mu);
        render = live.render;
        captured = live.capture;
        desktop_captured = live.desktop;
    }
    std::chrono::steady_clock::time_point origin = std::chrono::steady_clock::time_point::max();
    bool have_origin = false;
    const StreamClock clocks[] = {live.render_clock, live.capture_clock, live.desktop_clock};
    for (const StreamClock& clock : clocks) {
        if (clock.set && (!have_origin || clock.first < origin)) {
            origin = clock.first;
            have_origin = true;
        }
    }
    RunOptions stored = options;
    if (have_origin) {
        stored.render_pad_samples = lead_samples(live.render_clock, origin);
        stored.capture_pad_samples = lead_samples(live.capture_clock, origin);
        stored.desktop_pad_samples = lead_samples(live.desktop_clock, origin);
        pad_front(render, stored.render_pad_samples);
        pad_front(captured, stored.capture_pad_samples);
        pad_front(desktop_captured, stored.desktop_pad_samples);
    }
    stored.live = true;
    stored.render_shift_ms = 0;
    stored.playback_internal_rate_hz = playback_rate;
    stored.playback_internal_channels = playback_channels;
    stored.playback_period_frames = playback_period;
    stored.playback_volume = playback_volume;
    stored.capture_internal_rate_hz = capture_rate;
    stored.capture_internal_channels = capture_channels;
    stored.capture_volume = capture_volume;
    if (!opened_output.empty())
        stored.output_device = opened_output;
    if (!opened_input.empty())
        stored.input_device = opened_input;
    if (!opened_desktop.empty())
        stored.desktop_device = opened_desktop;
    if (stored.source == CaptureSource::desktop)
        return finish_offline(stored, std::move(render), {}, std::move(desktop_captured));
    return finish_offline(stored, std::move(render), std::move(captured), std::move(desktop_captured));
}

} // namespace

const char* scenario_name(Scenario scenario)
{
    switch (scenario) {
    case Scenario::silence: return "silence";
    case Scenario::far_only: return "far-only";
    case Scenario::near_only: return "near-only";
    case Scenario::double_talk: return "double-talk";
    case Scenario::transitions: return "transitions";
    }
    return "far-only";
}

bool scenario_from_name(const std::string& name, Scenario& scenario)
{
    const std::string text = lower_ascii(name);
    if (text == "silence") scenario = Scenario::silence;
    else if (text == "far-only" || text == "far_only") scenario = Scenario::far_only;
    else if (text == "near-only" || text == "near_only") scenario = Scenario::near_only;
    else if (text == "double-talk" || text == "double_talk") scenario = Scenario::double_talk;
    else if (text == "transitions") scenario = Scenario::transitions;
    else return false;
    return true;
}

BackendStatus backend_status()
{
    BackendStatus status;
    EchoCancellationProcessor::Config cfg;
    cfg.sample_rate_hz = kRate;
    cfg.channels = 1;
    cfg.agc = false;
    cfg.ns = false;
    auto aec = EchoCancellationProcessor::create(cfg);
    if (!aec)
        return status;
    status.installed = true;
    status.active = aec->active();
    status.name = aec->backend_name();
    return status;
}

nlohmann::json devices_json()
{
    const BackendStatus backend = backend_status();
    nlohmann::json capture = nlohmann::json::array();
    nlohmann::json playback = nlohmann::json::array();
    for (const DeviceInfo& device : enumerate_capture_devices()) {
        capture.push_back({
            {"name", device.name},
            {"default", device.is_default},
            {"loopback", device.is_loopback},
            {"channels", device.channels},
            {"sample_rate_hz", device.sample_rate},
        });
    }
    for (const DeviceInfo& device : enumerate_playback_devices()) {
        playback.push_back({
            {"name", device.name},
            {"default", device.is_default},
            {"channels", device.channels},
            {"sample_rate_hz", device.sample_rate},
        });
    }
    nlohmann::json loopback = nlohmann::json::array();
    for (const DeviceInfo& device : enumerate_loopback_devices()) {
        loopback.push_back({
            {"name", device.name},
            {"default", device.is_default},
            {"channels", device.channels},
            {"sample_rate_hz", device.sample_rate},
        });
    }
    return {
        {"schema", "pm.audio.aec3.devices/v1"},
        {"ok", true},
        {"backend", {
            {"installed", backend.installed},
            {"active", backend.active},
            {"name", backend.name},
        }},
        {"capture", capture},
        {"playback", playback},
        {"loopback", loopback},
    };
}

RunResult replay(const RunOptions& options)
{
    std::filesystem::path capture_path = options.capture_source;
    std::filesystem::path desktop_path = options.desktop_capture;
    std::filesystem::path render_path = options.render_source;
    if (!options.session.empty()) {
        if (capture_path.empty())
            capture_path = std::filesystem::exists(options.session / "capture-mic.wav")
                ? options.session / "capture-mic.wav"
                : options.session / "capture-raw.wav";
        if (desktop_path.empty())
            desktop_path = options.session / "capture-desktop.wav";
        if (render_path.empty())
            render_path = options.session / "render-reference.wav";
    }
    const bool want_mic = options.source != CaptureSource::desktop;
    const bool want_desktop = options.source != CaptureSource::mic;
    if (want_mic && capture_path.empty())
        return fail("EINVAL", "replay needs --capture or --session", 400);
    if (want_desktop && desktop_path.empty() && options.source == CaptureSource::mix)
        return fail("EINVAL", "mix replay needs --desktop-capture or a session with capture-desktop.wav", 400);
    if (!want_mic && desktop_path.empty())
        desktop_path = capture_path;
    if (want_desktop && desktop_path.empty())
        return fail("EINVAL", "desktop replay needs --desktop-capture, --capture, or --session", 400);

    std::vector<std::int16_t> mic;
    std::vector<std::int16_t> desktop;
    std::vector<std::int16_t> render;
    std::string error;
    if (want_mic && !load_s16(capture_path, mic, error))
        return fail("EIO", error, 400);
    if (want_desktop && !load_s16(desktop_path, desktop, error))
        return fail("EIO", error, 400);
    if (!render_path.empty() && std::filesystem::exists(render_path)) {
        if (!load_s16(render_path, render, error))
            return fail("EIO", error, 400);
    }
    RunOptions stored = options;
    stored.live = false;
    return finish_offline(stored, std::move(render), std::move(mic), std::move(desktop));
}

RunResult run(const RunOptions& options)
{
    RunOptions stored = options;
    stored.live = true;
    return run_live(stored);
}

RunResult inspect(const std::filesystem::path& session)
{
    const auto manifest_path = session / "manifest.json";
    std::ifstream in(manifest_path);
    if (!in)
        return fail("ENOENT", "session manifest was not found", 404);
    nlohmann::json manifest;
    try {
        in >> manifest;
    } catch (const std::exception& ex) {
        return fail("EIO", ex.what(), 400);
    }
    RunResult result;
    result.ok = manifest.value("complete", false);
    result.session_directory = session;
    result.summary = {
        {"schema", "pm.audio.aec3.inspect/v1"},
        {"ok", result.ok},
        {"session", path_to_utf8(session)},
        {"manifest", manifest},
    };
    if (!result.ok) {
        result.code = "EINCOMPLETE";
        result.error = "session manifest is incomplete";
        result.status = 409;
    }
    return result;
}

} // namespace pm::audio::aec3
