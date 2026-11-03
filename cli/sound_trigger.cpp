#include "sound_trigger.hpp"
#include "fs/path_sanitizer.hpp"

#include "pm/audio/devices.hpp"
#include "pm/audio/trigger/sound_trigger.hpp"

#include <nlohmann/json.hpp>

#include <algorithm>
#include <atomic>
#include <cctype>
#include <chrono>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <mutex>
#include <sstream>
#include <string>
#include <thread>
#include <vector>
#include "string/string_utils.hpp"

#if defined(_WIN32)
#  ifndef NOMINMAX
#    define NOMINMAX
#  endif
#  ifndef WIN32_LEAN_AND_MEAN
#    define WIN32_LEAN_AND_MEAN
#  endif
#  include <windows.h>
#else
#  include <csignal>
#endif

namespace {

using pm::audio::trigger::Detection;
using pm::audio::trigger::Engine;
using pm::audio::trigger::Profile;
using pm::audio::trigger::WavData;

std::atomic_bool g_listen_stop{false};

#if defined(_WIN32)
BOOL WINAPI listen_ctrl(DWORD)
{
    g_listen_stop.store(true);
    return TRUE;
}
#else
void listen_signal(int)
{
    g_listen_stop.store(true);
}
#endif

int fail(const std::string& code, const std::string& message, int status)
{
    nlohmann::json body = {
        {"ok", false},
        {"error", message},
        {"code", code},
        {"status", status},
    };
    std::cout << body.dump() << '\n';
    return 1;
}

void emit(const nlohmann::json& value)
{
    std::cout << value.dump() << '\n' << std::flush;
}

nlohmann::json detection_json(const Detection& hit)
{
    return {
        {"schema", "pm.sound.trigger/v1"},
        {"operation_id", hit.operation_id},
        {"trigger_id", hit.trigger_id},
        {"score", hit.score},
        {"score_kind", hit.score_kind},
        {"start_time_ms", hit.start_time_ms},
        {"end_time_ms", hit.end_time_ms},
        {"profile_version", hit.profile_version},
    };
}

nlohmann::json frame_json(const pm::audio::trigger::FrameMetrics& frame)
{
    nlohmann::json out = {
        {"schema", "pm.sound.frame/v1"},
        {"t_ms", frame.timestamp_ms},
        {"rms", frame.rms},
        {"noise_rms", frame.noise_rms},
        {"dominant_hz", frame.dominant_hz},
        {"global_peak_hz", frame.dominant_hz},
        {"predicted_peak_hz", frame.predicted_peak_hz},
        {"tracked_prominence", frame.tracked_prominence},
        {"concentration", frame.concentration},
        {"flatness", frame.flatness},
        {"band_ratio", frame.band_ratio},
        {"prominence", frame.prominence},
        {"state", pm::audio::trigger::ridge_state_name(frame.ridge_state)},
        {"hole_ms", frame.hole_ms},
        {"good_hop_fraction", frame.good_hop_fraction},
        {"decision_reason", pm::audio::trigger::ridge_reason_name(frame.ridge_reason)},
        {"active", frame.active},
        {"whistle", frame.whistle},
    };
    if (frame.tracked_peak_hz > 0.0f)
        out["tracked_peak_hz"] = frame.tracked_peak_hz;
    else
        out["tracked_peak_hz"] = nullptr;
    return out;
}

std::string xml_text(std::string text)
{
    for (std::size_t i = 0; i < text.size();) {
        const char ch = text[i];
        const char* replacement = nullptr;
        if (ch == '&') replacement = "&amp;";
        else if (ch == '<') replacement = "&lt;";
        else if (ch == '>') replacement = "&gt;";
        if (replacement == nullptr) {
            ++i;
            continue;
        }
        text.replace(i, 1, replacement);
        i += std::char_traits<char>::length(replacement);
    }
    return text;
}

bool write_inspect_plot(const std::filesystem::path& path,
                        const std::vector<pm::audio::trigger::FrameMetrics>& frames,
                        const std::string& title, std::string& error)
{
    if (frames.empty()) {
        error = "no frames to plot";
        return false;
    }
    const double t0 = static_cast<double>(frames.front().timestamp_ms);
    const double t1 = std::max(static_cast<double>(frames.back().timestamp_ms), t0 + 1.0);
    auto x_of = [&](std::uint64_t t) {
        return 48.0 + 900.0 * (static_cast<double>(t) - t0) / (t1 - t0);
    };
    auto y_hz = [](float hz) {
        const double clamped = std::clamp(static_cast<double>(hz), 500.0, 4500.0);
        return 200.0 - 170.0 * (clamped - 500.0) / 4000.0;
    };
    auto y_prom = [](float prominence) {
        const double clamped = std::clamp(static_cast<double>(prominence), 0.0, 80.0);
        return 390.0 - 140.0 * clamped / 80.0;
    };
    std::ostringstream svg;
    svg << "<svg xmlns=\"http://www.w3.org/2000/svg\" width=\"1000\" height=\"440\" "
           "viewBox=\"0 0 1000 440\">"
        << "<rect width=\"1000\" height=\"440\" fill=\"#1c1b19\"/>"
        << "<text x=\"48\" y=\"28\" fill=\"#d7d2c8\" font-family=\"Segoe UI,sans-serif\" "
           "font-size=\"16\">" << xml_text(title) << "</text>"
        << "<text x=\"48\" y=\"48\" fill=\"#8c867c\" font-family=\"Segoe UI,sans-serif\" "
           "font-size=\"12\">gray global peak, cyan acquiring, gold accepted tracker hop</text>"
        << "<line x1=\"48\" y1=\"200\" x2=\"948\" y2=\"200\" stroke=\"#3a3834\"/>"
        << "<line x1=\"48\" y1=\"30\" x2=\"48\" y2=\"200\" stroke=\"#3a3834\"/>"
        << "<line x1=\"48\" y1=\"390\" x2=\"948\" y2=\"390\" stroke=\"#3a3834\"/>"
        << "<line x1=\"48\" y1=\"" << y_prom(8.0f)
        << "\" x2=\"948\" y2=\"" << y_prom(8.0f)
        << "\" stroke=\"#c45c26\" stroke-dasharray=\"4 4\"/>";
    for (const auto& frame : frames) {
        if (frame.active && frame.dominant_hz > 0.0f) {
            svg << "<circle cx=\"" << x_of(frame.timestamp_ms) << "\" cy=\""
                << y_hz(frame.dominant_hz)
                << "\" r=\"1.2\" fill=\"#6a6560\" fill-opacity=\"0.55\"/>";
        }
        if (frame.tracked_peak_hz > 0.0f) {
            const bool tracking = frame.whistle;
            const bool acquiring = frame.ridge_state == pm::audio::trigger::RidgeState::acquiring;
            const char* color = tracking ? "#e6b84d" : (acquiring ? "#5ec8d8" : "#e6b84d");
            svg << "<circle cx=\"" << x_of(frame.timestamp_ms) << "\" cy=\""
                << y_hz(frame.tracked_peak_hz) << "\" r=\""
                << (tracking ? 2.8 : 2.0) << "\" fill=\"" << color << "\"/>";
            svg << "<circle cx=\"" << x_of(frame.timestamp_ms) << "\" cy=\""
                << y_prom(frame.tracked_prominence) << "\" r=\""
                << (tracking ? 2.2 : 1.6) << "\" fill=\"" << color << "\"/>";
        }
        if (frame.ridge_reason == pm::audio::trigger::RidgeReason::accepted) {
            svg << "<circle cx=\"" << x_of(frame.timestamp_ms)
                << "\" cy=\"18\" r=\"3.5\" fill=\"none\" stroke=\"#e6b84d\"/>";
        } else if (frame.ridge_reason == pm::audio::trigger::RidgeReason::rejected_sparse
                   || frame.ridge_reason == pm::audio::trigger::RidgeReason::rejected_duration
                   || frame.ridge_reason == pm::audio::trigger::RidgeReason::rejected_overlong) {
            svg << "<circle cx=\"" << x_of(frame.timestamp_ms)
                << "\" cy=\"18\" r=\"3.5\" fill=\"none\" stroke=\"#c45c26\"/>";
        }
    }
    svg << "</svg>\n";
    std::ofstream out(path, std::ios::binary | std::ios::trunc);
    if (!out) {
        error = "cannot write plot: " + media::path::path_to_utf8(path);
        return false;
    }
    out << svg.str();
    return static_cast<bool>(out);
}

bool read_wav(const std::string& path, WavData& wav, std::string& error)
{
    return pm::audio::trigger::read_wav_mono16(path, wav, error);
}

bool read_inputs(const std::vector<std::string>& paths, std::vector<WavData>& output, std::string& error)
{
    for (const auto& path : paths) {
        WavData wav;
        if (!read_wav(path, wav, error))
            return false;
        output.push_back(std::move(wav));
    }
    return true;
}

bool resolve_profile(const std::string& profile_path, const std::string& builtin,
                     Profile& profile, std::string& error)
{
    if (!profile_path.empty())
        return pm::audio::trigger::load_profile(profile_path, profile, error);
    std::string name = builtin;
    name = media::strutil::lower_ascii(std::move(name));
    if (name.empty() || name == "double-whistle") {
        profile = {};
        profile.trigger_id = "double-whistle";
        profile.detector = "double-whistle";
        return true;
    }
    error = "unknown built-in detector: " + builtin;
    return false;
}

std::vector<Detection> match_wav(Engine& engine, const WavData& wav)
{
    std::vector<Detection> hits;
    constexpr std::size_t block = 320;
    for (std::size_t offset = 0; offset < wav.mono.size(); offset += block) {
        const std::size_t count = std::min(block, wav.mono.size() - offset);
        auto chunk = engine.process(wav.mono.data() + offset, count, wav.sample_rate,
                                    offset * 1000 / wav.sample_rate);
        hits.insert(hits.end(), std::make_move_iterator(chunk.begin()),
                    std::make_move_iterator(chunk.end()));
    }
    auto tail = engine.finish(wav.mono.size() * 1000 / std::max<std::uint32_t>(wav.sample_rate, 1));
    hits.insert(hits.end(), std::make_move_iterator(tail.begin()),
                std::make_move_iterator(tail.end()));
    return hits;
}

int cmd_inspect(const std::string& input, bool frames_only, const std::string& frames_out,
                const std::string& plot)
{
    WavData wav;
    std::string error;
    if (!read_wav(input, wav, error))
        return fail("EIO", error, 400);
    const auto frames = pm::audio::trigger::inspect_pcm(
        wav.mono.data(), wav.mono.size(), wav.sample_rate);
    if (!plot.empty()) {
        if (!write_inspect_plot(plot, frames, input, error))
            return fail("EIO", error, 400);
    }
    if (frames_only) {
        std::ofstream file;
        std::ostream* sink = &std::cout;
        if (!frames_out.empty()) {
            file.open(frames_out, std::ios::binary | std::ios::trunc);
            if (!file)
                return fail("EIO", "cannot write " + frames_out, 400);
            sink = &file;
        }
        for (const auto& frame : frames)
            *sink << frame_json(frame).dump() << '\n';
        return frames.empty() ? 2 : 0;
    }
    std::size_t active = 0;
    std::size_t whistle = 0;
    float peak_rms = 0.0f;
    float peak_hz = 0.0f;
    for (const auto& frame : frames) {
        if (frame.active)
            ++active;
        if (frame.whistle)
            ++whistle;
        peak_rms = std::max(peak_rms, frame.rms);
        if (frame.concentration >= 0.28f)
            peak_hz = frame.dominant_hz;
    }
    emit({
        {"schema", "pm.sound.inspect/v1"},
        {"sample_rate", wav.sample_rate},
        {"samples", wav.mono.size()},
        {"frames", frames.size()},
        {"whistle_frames", whistle},
        {"active_frames", active},
        {"peak_rms", peak_rms},
        {"peak_hz", peak_hz},
    });
    return frames.empty() ? 2 : 0;
}

int cmd_match(const std::string& input, const std::string& profile_path, const std::string& builtin)
{
    Profile profile;
    WavData wav;
    std::string error;
    if (!resolve_profile(profile_path, builtin, profile, error) || !read_wav(input, wav, error))
        return fail("EIO", error, 400);
    Engine engine;
    if (!engine.add_profile(std::move(profile), error))
        return fail("EINVAL", error, 400);
    const auto hits = match_wav(engine, wav);
    for (const auto& hit : hits)
        emit(detection_json(hit));
    if (hits.empty()) {
        emit({{"schema", "pm.sound.no-match/v1"}, {"input", input}});
        return 2;
    }
    return 0;
}

int cmd_train(const std::string& id, const std::vector<std::string>& samples,
              const std::vector<std::string>& ambient, const std::string& out_path)
{
    std::vector<WavData> positives;
    std::vector<WavData> negatives;
    std::string error;
    if (!read_inputs(samples, positives, error) || !read_inputs(ambient, negatives, error))
        return fail("EIO", error, 400);
    const auto result = pm::audio::trigger::train_profile(id, positives, negatives);
    if (!result.ok) {
        std::cerr << result.error << '\n';
        return 2;
    }
    if (!pm::audio::trigger::save_profile(result.profile, out_path, error))
        return fail("EIO", error, 400);
    emit({
        {"schema", "pm.sound.training/v1"},
        {"trigger_id", result.profile.trigger_id},
        {"accepted", result.accepted},
        {"rejected", result.rejected},
        {"positive_calibration", "leave-one-out"},
        {"positive_max", result.positive_max},
        {"negative_min", result.negative_min},
        {"threshold", result.profile.threshold},
        {"margin", result.profile.margin},
        {"profile", out_path},
    });
    return 0;
}

int cmd_listen(const std::string& profile_path, const std::string& builtin, const std::string& source,
               const std::string& device, int duration_ms, bool announce)
{
    std::string which = source;
    which = media::strutil::lower_ascii(std::move(which));
    if (which == "mix")
        return fail("ENOTSUP", "mix capture stays on the host sound-trigger command", 400);

    Profile profile;
    std::string error;
    if (!resolve_profile(profile_path, builtin, profile, error))
        return fail("EINVAL", error, 400);
    const std::string trigger_id = profile.trigger_id;
    Engine engine;
    if (!engine.add_profile(std::move(profile), error))
        return fail("EINVAL", error, 400);

    std::mutex mu;
    std::vector<std::int16_t> pending;
    g_listen_stop.store(false);
#if defined(_WIN32)
    ::SetConsoleCtrlHandler(listen_ctrl, TRUE);
#else
    std::signal(SIGINT, listen_signal);
#endif

    auto on_pcm = [&](const std::int16_t* pcm, std::size_t frames) {
        if (!pcm || frames == 0)
            return;
        std::lock_guard<std::mutex> lock(mu);
        if (pending.size() + frames > 32000)
            return;
        pending.insert(pending.end(), pcm, pcm + frames);
    };

    pm::audio::AudioInput mic;
    pm::audio::DesktopAudioCapture loopback;
    try {
        if (which == "desktop") {
            if (!loopback.start(on_pcm, device, error, pm::audio::trigger::k_default_sample_rate, 1))
                return fail("EIO", error.empty() ? "could not open loopback" : error, 400);
        } else {
            mic.start(on_pcm, device, pm::audio::trigger::k_default_sample_rate, 1);
        }
    } catch (const std::exception& ex) {
        return fail("EIO", ex.what(), 400);
    }

    if (announce) {
        const std::string where = device.empty() ? std::string("system default") : device;
        std::cerr << "sound-trigger: listening on " << where << " (" << which << ", "
                  << trigger_id << "). Ctrl+C to stop.\n";
    }

    const auto started = std::chrono::steady_clock::now();
    const auto deadline = duration_ms > 0
        ? started + std::chrono::milliseconds(duration_ms)
        : std::chrono::steady_clock::time_point::max();
    std::uint64_t seen = 0;
    while (!g_listen_stop.load() && std::chrono::steady_clock::now() < deadline) {
        std::this_thread::sleep_for(std::chrono::milliseconds(40));
        std::vector<std::int16_t> chunk;
        {
            std::lock_guard<std::mutex> lock(mu);
            chunk.swap(pending);
        }
        if (chunk.empty())
            continue;
        const auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(
            std::chrono::steady_clock::now() - started).count();
        auto hits = engine.process(chunk.data(), chunk.size(),
                                   pm::audio::trigger::k_default_sample_rate,
                                   static_cast<std::uint64_t>(elapsed));
        seen += chunk.size();
        for (const auto& hit : hits) {
            if (announce) {
                std::cerr << "sound-trigger: found " << hit.trigger_id << " "
                          << hit.start_time_ms << "-" << hit.end_time_ms
                          << " ms score=" << hit.score << '\n';
            }
            emit(detection_json(hit));
        }
    }
    if (which == "desktop")
        loopback.stop();
    else
        mic.stop();
    const auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::steady_clock::now() - started).count();
    auto tail = engine.finish(static_cast<std::uint64_t>(std::max<std::int64_t>(elapsed, 0)));
    for (const auto& hit : tail)
        emit(detection_json(hit));
    (void)seen;
    return 0;
}

struct Options {
    CLI::App* inspect = nullptr;
    CLI::App* match = nullptr;
    CLI::App* train = nullptr;
    CLI::App* listen = nullptr;
    CLI::App* test = nullptr;
    std::string input;
    std::string profile;
    std::string builtin = "double-whistle";
    std::string frames_out;
    std::string plot;
    std::string id;
    std::string out;
    std::string source = "mic";
    std::string device;
    std::vector<std::string> samples;
    std::vector<std::string> ambient;
    int duration_ms = 0;
    bool frames = false;
};

Options& options()
{
    static Options opt;
    return opt;
}

} // namespace

SoundTriggerCli add_sound_trigger(CLI::App& app)
{
    auto& opt = options();
    SoundTriggerCli cli;
    cli.root = app.add_subcommand("sound-trigger", "Inspect, train, match, and listen for sound patterns");
    cli.root->require_subcommand(1);

    opt.inspect = cli.root->add_subcommand("inspect", "Inspect whistle features in a WAV");
    opt.inspect->add_option("input", opt.input, "WAV to inspect")->required();
    opt.inspect->add_flag("--json", "Emit the summary as one JSON object");
    opt.inspect->add_flag("--frames", opt.frames, "Emit one JSON object per hop");
    opt.inspect->add_option("--out", opt.frames_out, "Write --frames JSONL here");
    opt.inspect->add_option("--plot", opt.plot, "Write an SVG of the ridge");

    opt.match = cli.root->add_subcommand("match", "Match a WAV against the double whistle or a profile");
    opt.match->add_option("input", opt.input, "WAV to match")->required();
    opt.match->add_option("--profile", opt.profile, "Trained profile JSON");
    opt.match->add_option("--builtin", opt.builtin, "Built-in detector")->default_str("double-whistle");
    opt.match->add_flag("--jsonl,--json", "Emit JSONL envelopes");

    opt.train = cli.root->add_subcommand("train", "Train a template-dtw profile from WAV takes");
    opt.train->add_option("--id", opt.id, "Trigger id")->required();
    opt.train->add_option("--samples", opt.samples, "Eight or more positive takes")->required()->expected(8, -1);
    opt.train->add_option("--ambient", opt.ambient, "Ambient files that contain an event")->required();
    opt.train->add_option("--out", opt.out, "Profile JSON to write")->required();
    opt.train->add_flag("--json", "Emit the training summary as JSON");

    opt.listen = cli.root->add_subcommand("listen", "Listen on the mic or desktop loopback");
    opt.test = cli.root->add_subcommand("test", "Listen until Ctrl+C and print each double whistle");
    for (CLI::App* sub : {opt.listen, opt.test}) {
        sub->add_option("--profile", opt.profile, "Trained profile JSON");
        sub->add_option("--builtin", opt.builtin, "Built-in detector")->default_str("double-whistle");
        sub->add_option("--source", opt.source, "mic or desktop")->default_str("mic");
        sub->add_option("--input", opt.device, "Capture device substring");
        sub->add_flag("--jsonl,--json", "Emit JSONL detections");
    }
    opt.listen->add_option("--duration-ms", opt.duration_ms, "Stop after this many milliseconds");
    return cli;
}

int SoundTriggerCli::run() const
{
    if (!root || !root->parsed())
        return -1;
    const auto& opt = options();
    if (opt.inspect->parsed())
        return cmd_inspect(opt.input, opt.frames, opt.frames_out, opt.plot);
    if (opt.match->parsed())
        return cmd_match(opt.input, opt.profile, opt.builtin);
    if (opt.train->parsed())
        return cmd_train(opt.id, opt.samples, opt.ambient, opt.out);
    if (opt.listen->parsed())
        return cmd_listen(opt.profile, opt.builtin, opt.source, opt.device, opt.duration_ms, false);
    if (opt.test->parsed())
        return cmd_listen(opt.profile, opt.builtin, opt.source, opt.device, 0, true);
    return 2;
}
