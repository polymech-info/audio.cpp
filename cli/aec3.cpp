#include "aec3.hpp"

#include "pm/audio/aec3_lab.hpp"
#include "pm/audio/text.hpp"

#include <nlohmann/json.hpp>

#include <iostream>
#include <string>

#if defined(PM_AUDIO_AEC3_BACKEND_WEBRTC)
void install_aec3_webrtc_backend();
#endif

namespace {

using pm::audio::aec3::DelayMode;
using pm::audio::aec3::RunOptions;
using pm::audio::aec3::Scenario;

int fail(const std::string& code, const std::string& message, int status)
{
    nlohmann::json body = {
        {"ok", false},
        {"error", message},
        {"code", code},
        {"status", status},
    };
    std::cout << body.dump() << '\n';
    return status == 400 ? 2 : 1;
}

int emit(const pm::audio::aec3::RunResult& result)
{
    if (!result.ok) {
        nlohmann::json body = {
            {"ok", false},
            {"error", result.error},
            {"code", result.code},
            {"status", result.status},
        };
        if (!result.session_directory.empty())
            body["session"] = pm::audio::path_to_utf8(result.session_directory);
        std::cout << body.dump() << '\n';
        return result.status == 400 ? 2 : 1;
    }
    std::cout << result.summary.dump() << '\n';
    return 0;
}

struct Options {
    CLI::App* devices = nullptr;
    CLI::App* calibrate = nullptr;
    CLI::App* run = nullptr;
    CLI::App* replay = nullptr;
    CLI::App* inspect = nullptr;
    std::string input;
    std::string output;
    std::string render;
    std::string capture;
    std::string near_path;
    std::string session;
    std::string output_dir;
    std::string scenario = "far-only";
    std::string source = "mic";
    std::string desktop_device;
    std::string desktop_capture;
    float mic_gain = 1.f;
    float desktop_gain = 1.f;
    std::string delay = "automatic";
    int delay_ms = 0;
    int render_shift_ms = 0;
    int seconds = 20;
    bool no_aec = false;
    bool ns = false;
    bool agc = false;
};

Options& options()
{
    static Options opt;
    return opt;
}

void install_backend()
{
#if defined(PM_AUDIO_AEC3_BACKEND_WEBRTC)
    install_aec3_webrtc_backend();
#endif
}

RunOptions make_options()
{
    const Options& opt = options();
    RunOptions run;
    run.input_device = opt.input;
    run.output_device = opt.output;
    run.output_directory = pm::audio::path_from_utf8(opt.output_dir);
    run.render_source = pm::audio::path_from_utf8(opt.render);
    run.capture_source = pm::audio::path_from_utf8(opt.capture);
    run.desktop_capture = pm::audio::path_from_utf8(opt.desktop_capture);
    run.near_source = pm::audio::path_from_utf8(opt.near_path);
    run.session = pm::audio::path_from_utf8(opt.session);
    run.desktop_device = opt.desktop_device;
    run.mic_gain = opt.mic_gain;
    run.desktop_gain = opt.desktop_gain;
    run.delay_ms = opt.delay_ms;
    run.render_shift_ms = opt.render_shift_ms;
    run.duration_seconds = opt.seconds;
    run.aec_enabled = !opt.no_aec;
    run.ns_enabled = opt.ns;
    run.agc_enabled = opt.agc;
    run.delay_mode = opt.delay == "fixed" ? DelayMode::fixed : DelayMode::automatic;
    return run;
}

void add_device_options(CLI::App* command)
{
    auto& opt = options();
    command->add_option("--input", opt.input, "Microphone device name. Empty uses the system default");
    command->add_option("--output", opt.output, "Playback device name. Empty uses the system default");
    command->add_option("--desktop", opt.desktop_device, "Loopback playback device. Empty uses --output, or the system default speaker");
    command->add_option("--source", opt.source, "mic, desktop, or mix");
}

} // namespace

Aec3Cli add_aec3(CLI::App& app)
{
    auto& opt = options();
    Aec3Cli cli;
    cli.root = app.add_subcommand("aec3", "Measure acoustic echo cancellation");
    cli.root->require_subcommand(1);

    opt.devices = cli.root->add_subcommand("devices", "List devices and the AEC backend");
    opt.calibrate = cli.root->add_subcommand("calibrate", "Record a short noise and lag check");
    opt.run = cli.root->add_subcommand("run", "Play a render file and record the microphone");
    opt.replay = cli.root->add_subcommand("replay", "Process saved capture and render WAVs");
    opt.inspect = cli.root->add_subcommand("inspect", "Print a session manifest");

    add_device_options(opt.calibrate);
    add_device_options(opt.run);
    opt.calibrate->add_option("--render", opt.render, "Far-end WAV");
    opt.calibrate->add_option("--out", opt.output_dir, "Session directory");
    opt.calibrate->add_option("--seconds", opt.seconds, "Capture length");
    opt.calibrate->add_flag("--no-aec", opt.no_aec, "Record without echo cancellation");

    opt.run->add_option("--render", opt.render, "Far-end WAV");
    opt.run->add_option("--near", opt.near_path, "Optional near-end reference to copy into the session");
    opt.run->add_option("--out", opt.output_dir, "Session directory");
    opt.run->add_option("--scenario", opt.scenario, "silence, far-only, near-only, double-talk, transitions");
    opt.run->add_option("--delay", opt.delay, "automatic or fixed");
    opt.run->add_option("--delay-ms", opt.delay_ms, "Fixed stream delay hint");
    opt.run->add_option("--render-shift-ms", opt.render_shift_ms, "Insert silence so the reference lags the file");
    opt.run->add_option("--seconds", opt.seconds, "Maximum capture length");
    opt.run->add_flag("--no-aec", opt.no_aec, "Record without echo cancellation");
    opt.run->add_flag("--ns", opt.ns, "Enable noise suppression");
    opt.run->add_flag("--agc", opt.agc, "Enable gain control");

    opt.replay->add_option("--session", opt.session, "Existing session directory");
    opt.replay->add_option("--capture", opt.capture, "Microphone WAV");
    opt.replay->add_option("--desktop-capture", opt.desktop_capture, "Desktop loopback WAV");
    opt.replay->add_option("--render", opt.render, "Far-end WAV");
    opt.replay->add_option("--source", opt.source, "mic, desktop, or mix");
    opt.replay->add_option("--mic-gain", opt.mic_gain, "Microphone gain applied after AEC");
    opt.replay->add_option("--desktop-gain", opt.desktop_gain, "Desktop gain applied after AEC");
    opt.replay->add_option("--near", opt.near_path, "Optional near-end reference to copy into the session");
    opt.replay->add_option("--out", opt.output_dir, "Session directory");
    opt.replay->add_option("--scenario", opt.scenario, "silence, far-only, near-only, double-talk, transitions");
    opt.replay->add_option("--delay", opt.delay, "automatic or fixed");
    opt.replay->add_option("--delay-ms", opt.delay_ms, "Fixed stream delay hint");
    opt.replay->add_option("--render-shift-ms", opt.render_shift_ms, "Insert silence so the reference lags the file");
    opt.replay->add_flag("--no-aec", opt.no_aec, "Copy framed audio without echo cancellation");
    opt.replay->add_flag("--ns", opt.ns, "Enable noise suppression");
    opt.replay->add_flag("--agc", opt.agc, "Enable gain control");

    opt.inspect->add_option("--session", opt.session, "Session directory")->required();
    return cli;
}

int Aec3Cli::run() const
{
    install_backend();
    auto& opt = options();
    if (opt.devices->parsed()) {
        std::cout << pm::audio::aec3::devices_json().dump() << '\n';
        return 0;
    }
    if (opt.inspect->parsed())
        return emit(pm::audio::aec3::inspect(pm::audio::path_from_utf8(opt.session)));

    RunOptions run = make_options();
    const std::string source_token = pm::audio::lower_ascii(opt.source);
    if (source_token != "mic" && source_token != "microphone"
        && source_token != "desktop" && source_token != "system" && source_token != "loopback"
        && source_token != "mix" && source_token != "both")
        return fail("EINVAL", "source must be mic, desktop, or mix", 400);
    run.source = pm::audio::parse_capture_source(opt.source);
    if (!opt.scenario.empty() && !pm::audio::aec3::scenario_from_name(opt.scenario, run.scenario))
        return fail("EINVAL", "unknown scenario", 400);
    if (opt.delay != "automatic" && opt.delay != "fixed")
        return fail("EINVAL", "delay must be automatic or fixed", 400);
    if (opt.delay == "fixed")
        run.delay_mode = DelayMode::fixed;

    if (opt.calibrate->parsed()) {
        run.scenario = Scenario::far_only;
        run.live = true;
        if (run.duration_seconds == 20)
            run.duration_seconds = 3;
        return emit(pm::audio::aec3::run(run));
    }
    if (opt.run->parsed())
        return emit(pm::audio::aec3::run(run));
    if (opt.replay->parsed())
        return emit(pm::audio::aec3::replay(run));
    return fail("EINVAL", "aec3 needs a subcommand", 400);
}
