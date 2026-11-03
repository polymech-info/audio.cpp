#include "pm/audio/devices.hpp"
#if defined(PM_AUDIO_SOUND_TRIGGER) && PM_AUDIO_SOUND_TRIGGER
#include "sound_trigger.hpp"
#endif
#include "pm/audio/pcm_record.hpp"
#include "pm/audio/record_session.hpp"
#include "pm/audio/filter/audio_filter.hpp"
#include "pm/audio/filter/models.hpp"
#include "pm/audio/session/audio_session_selftest.hpp"
#include "pm/audio/stt/wav.hpp"
#include "pm/audio/stt/whisper.hpp"
#include "pm/audio/tts/moss_tts.hpp"
#include "pm/audio/tts/text.hpp"
#include "pm/audio/subs/subtitle_writer.hpp"
#include "pm/audio/voice/voice_match.hpp"
#include "pm/audio/voice/voice_pipeline.hpp"

#include <CLI/CLI.hpp>
#include <nlohmann/json.hpp>

#include <atomic>
#include <cctype>
#include <chrono>
#include <filesystem>
#include <iostream>
#include <string>
#include <vector>
#include "string/string_utils.hpp"

namespace {

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

int cmd_devices()
{
    const auto capture = pm::audio::enumerate_capture_devices();
    const auto playback = pm::audio::enumerate_playback_devices();
    nlohmann::json body = {
        {"ok", true},
        {"capture", capture.size()},
        {"playback", playback.size()},
    };
    std::cout << body.dump() << '\n';
    return 0;
}

int cmd_selftest(const std::string& suite)
{
    const pm::audio::session::SelfTestReport report = pm::audio::session::run_selftest(suite);
    std::cout << report.json << '\n';
    return report.ok ? 0 : 1;
}

int cmd_match(const std::string& text, const std::string& wake, bool wake_off)
{
    pm::audio::voice::MatchConfig config;
    if (wake_off || wake.empty())
        pm::audio::voice::apply_wake_phrase(config, "");
    else {
        config.wake_prefixes = {wake};
        config.wake_required = true;
    }
    const pm::audio::voice::MatchResult matched = pm::audio::voice::match_voice_text(text, config);
    nlohmann::json body = {
        {"ok", true},
        {"matched", matched.matched},
        {"normalized", matched.normalized_text},
        {"command", matched.command_text},
        {"wake", matched.wake_prefix},
        {"control", pm::audio::voice::control_intent_name(matched.control)},
    };
    std::cout << body.dump() << '\n';
    return 0;
}

int cmd_subs(const std::string& format, const std::string& out, const std::string& text,
             std::int64_t t0, std::int64_t t1)
{
    const std::vector<pm::subs::Format> formats = pm::subs::parse_format_list(format);
    if (formats.size() != 1)
        return fail("EINVAL", "format must be srt, vtt, or sbv", 400);
    pm::subs::Cue cue;
    cue.text = text;
    cue.t0_ms = t0;
    cue.t1_ms = t1;
    std::string err;
    if (!pm::subs::write_format(formats[0], out, {cue}, {}, err))
        return fail("EIO", err.empty() ? "could not write subtitles" : err, 500);
    std::vector<pm::subs::Cue> read_back;
    if (formats[0] != pm::subs::Format::sbv && !pm::subs::read_cues_file(out, read_back, err))
        return fail("EIO", err.empty() ? "could not read subtitles" : err, 500);
    nlohmann::json body = {
        {"ok", true},
        {"path", out},
        {"format", format},
        {"cues", static_cast<int>(formats[0] == pm::subs::Format::sbv ? 1 : read_back.size())},
    };
    std::cout << body.dump() << '\n';
    return 0;
}

int cmd_decode(const std::string& src, const std::string& dst, std::uint32_t rate)
{
    const pm::audio::filter::Result result =
        pm::audio::filter::decode_to_wav(src, dst, rate);
    nlohmann::json body = {
        {"ok", result.ok},
        {"kind", result.kind},
        {"sample_rate", result.sample_rate},
        {"channels", result.channels},
        {"frames", result.frames},
    };
    if (!result.ok) {
        body["error"] = result.error;
        body["code"] = "EIO";
        body["status"] = 400;
    }
    std::cout << body.dump() << '\n';
    return result.ok ? 0 : 1;
}

int cmd_lang(const std::string& text)
{
    nlohmann::json body = {
        {"ok", true},
        {"language", pm::stt::whisper_normalize_language(text)},
    };
    std::cout << body.dump() << '\n';
    return 0;
}

int cmd_wav_write(const std::string& dst, int frames, int rate)
{
    if (frames < 0)
        return fail("EINVAL", "frames must be >= 0", 400);
    const std::vector<short> samples(static_cast<std::size_t>(frames), 0);
    if (!pm::stt::write_pcm_wav(dst, samples.data(), samples.size(), rate))
        return fail("EIO", "could not write wav", 400);
    nlohmann::json body = {
        {"ok", true},
        {"path", dst},
        {"frames", frames},
        {"rate", rate},
    };
    std::cout << body.dump() << '\n';
    return 0;
}

int cmd_wav_read(const std::string& src)
{
    try {
        const std::vector<float> pcm = pm::stt::read_wav_16k(src);
        nlohmann::json body = {
            {"ok", true},
            {"frames", pcm.size()},
            {"rate", 16000},
        };
        std::cout << body.dump() << '\n';
        return 0;
    } catch (const std::exception& ex) {
        return fail("EIO", ex.what(), 400);
    }
}

int cmd_probe(const std::string& model)
{
    const pm::stt::WhisperModelProbe probe = pm::stt::whisper_probe_model_path(model);
    const bool found = !probe.path.empty();
    nlohmann::json body = {
        {"ok", found},
        {"model_id", probe.model_id},
        {"filename", probe.filename},
        {"path", probe.path},
        {"probed", probe.probed},
    };
    if (!found) {
        body["error"] = pm::stt::whisper_model_not_found_message(probe);
        body["code"] = "ENOENT";
        body["status"] = 404;
    }
    std::cout << body.dump() << '\n';
    return found ? 0 : 1;
}

int cmd_speak(const std::string& text, const std::string& src)
{
    pm::tts::TtsTextInputResult result;
    std::string err;
    const std::string input = src.empty() ? text : src;
    if (!pm::tts::resolve_tts_text(input, result, err))
        return fail("EINVAL", err.empty() ? "could not prepare text" : err, 400);
    nlohmann::json body = {
        {"ok", true},
        {"text", result.text},
        {"from_file", result.from_file},
        {"preparser", result.preparser_id},
    };
    std::cout << body.dump() << '\n';
    return 0;
}

int cmd_moss()
{
    auto& engine = pm::tts::MossNanoEngine::global();
    const int rc = engine.ensure({}, {}, {});
    const bool ok = rc != 0 && !engine.is_loaded();
    nlohmann::json body = {
        {"ok", ok},
        {"loaded", engine.is_loaded()},
        {"code", rc},
    };
    std::cout << body.dump() << '\n';
    return ok ? 0 : 1;
}

struct VoiceSink {
    std::string command;
    bool triggered = false;
};

void on_voice_trigger(void* context, const pm::audio::voice::TriggerEvent& event)
{
    auto* sink = static_cast<VoiceSink*>(context);
    sink->triggered = true;
    sink->command = event.text;
}

bool voice_transcribe(void*,
                      const std::int16_t*,
                      std::size_t,
                      const std::atomic_bool&,
                      pm::audio::voice::Transcript& transcript,
                      std::string&)
{
    transcript.text = "tanit what time";
    transcript.confidence = 1.f;
    return true;
}

int cmd_voice()
{
    using pm::audio::voice::PipelineConfig;
    using pm::audio::voice::PcmBuffer;
    using pm::audio::voice::Transcript;
    using pm::audio::voice::VoiceAdapters;
    using pm::audio::voice::VoicePipeline;

    VoiceSink sink;
    PipelineConfig config;
    config.capture_format.sample_rate = 16000;
    config.capture_format.channels = 1;
    config.match.wake_prefixes = {"tanit"};
    config.match.wake_required = true;
    config.vad.rms_threshold = 200.f;
    config.vad.speech_start_ms = 20;
    config.vad.silence_ms = 40;
    config.vad.min_utterance_ms = 20;
    config.vad.preroll_ms = 0;
    config.enhance_completed_utterances = false;

    VoiceAdapters adapters;
    adapters.routes.context = &sink;
    adapters.routes.on_trigger = on_voice_trigger;
    adapters.transcription.transcribe = voice_transcribe;

    VoicePipeline pipeline(std::move(config), adapters);
    pipeline.inject_text(Transcript{"tanit what time", 1.f, "en"});
    const bool text_ok = sink.triggered && sink.command == "what time";

    sink = {};
    std::string error;
    if (!pipeline.start_replay(error))
        return fail("EIO", error, 400);
    PcmBuffer pcm;
    pcm.format.sample_rate = 16000;
    pcm.format.channels = 1;
    pcm.samples.assign(400, 8000);
    pcm.samples.insert(pcm.samples.end(), 800, 0);
    if (!pipeline.replay_pcm(pcm, std::chrono::seconds(2), error)) {
        pipeline.stop();
        return fail("EIO", error, 400);
    }
    pipeline.stop();
    nlohmann::json body = {
        {"ok", text_ok && sink.triggered && sink.command == "what time"},
        {"text_command", "what time"},
        {"replay_command", sink.command},
        {"replay", sink.triggered},
    };
    if (!body["ok"].get<bool>()) {
        body["error"] = "voice pipeline did not route the wake phrase";
        body["code"] = "EIO";
        body["status"] = 400;
    }
    std::cout << body.dump() << '\n';
    return body["ok"].get<bool>() ? 0 : 1;
}

int cmd_record_wav(const std::string& dst)
{
    pm::audio::PcmRecordBuffer buffer;
    buffer.set_format(48000, 2);
    std::vector<int16_t> pcm(480 * 2, 1000);
    buffer.push(pcm.data(), 480);
    if (!buffer.write_wav(dst))
        return fail("EIO", "could not write wav", 400);
    std::string ext = std::filesystem::path(dst).extension().string();
    ext = media::strutil::lower_ascii(std::move(ext));
    const bool aac = pm::audio::record_container_from_extension(ext) == pm::audio::RecordContainer::Aac;
    nlohmann::json body = {
        {"ok", true},
        {"frames", buffer.frame_count()},
        {"bytes", buffer.data_bytes()},
        {"container", aac ? "aac" : "wav"},
        {"aac", pm::audio::aac_record_supported()},
    };
    std::cout << body.dump() << '\n';
    return 0;
}

int cmd_record_session(const std::string& dir)
{
    static std::filesystem::path session_dir;
    session_dir = dir;
    pm::audio::record_session::set_record_session_dir([] { return session_dir; });
    pm::audio::record_session::Owner owner;
    pm::audio::record_session::StartInfo info;
    info.dst = "clip.wav";
    info.format = "s16le stereo 48000 Hz";
    std::string err;
    if (!owner.acquire(info, err))
        return fail("EIO", err.empty() ? "could not acquire session" : err, 400);
    owner.set_samples(480);
    owner.set_device("test");
    if (!pm::audio::record_session::request_stop(err))
        return fail("EIO", err.empty() ? "could not stop session" : err, 400);
    pm::audio::record_session::Status status;
    if (!pm::audio::record_session::read_status(status, err))
        return fail("EIO", err.empty() ? "could not read session" : err, 400);
    nlohmann::json body = {
        {"ok", true},
        {"active", status.active},
        {"stop", owner.stop_requested()},
        {"samples", status.samples},
        {"device", status.device},
        {"aac", pm::audio::aac_record_supported()},
    };
    std::cout << body.dump() << '\n';
    return 0;
}

int cmd_runtimes()
{
    const pm::audio::filter::RuntimeInfo info = pm::audio::filter::runtime_info();
    nlohmann::json body = {
        {"ok", true},
        {"onnx", info.onnx},
        {"onnx_root", info.onnx_root},
        {"whisper", info.whisper},
        {"whisper_root", info.whisper_root},
        {"tanit_root", info.tanit_root},
        {"deepfilter", pm::audio::filter::available(pm::audio::filter::Kind::DeepFilter)},
        {"gtcrn", pm::audio::filter::available(pm::audio::filter::Kind::Gtcrn)},
    };
    std::cout << body.dump() << '\n';
    return 0;
}

} // namespace

int main(int argc, char** argv)
{
    CLI::App app{"pm-audio"};
    app.require_subcommand(1);

    std::string suite = "all";
    auto* selftest = app.add_subcommand("selftest", "Run the in-process session checks");
    selftest->add_option("--suite", suite, "all, lease, route, event, or graph");

    std::string match_text;
    std::string wake;
    bool wake_off = false;
    auto* match = app.add_subcommand("match", "Normalize and match a transcript");
    match->add_option("--text", match_text, "Transcript text")->required();
    match->add_option("--wake", wake, "Required wake prefix");
    match->add_flag("--wake-off", wake_off, "The whole utterance is the command");

    std::string format = "srt";
    std::string out;
    std::string cue_text;
    std::int64_t t0 = 0;
    std::int64_t t1 = 1000;
    auto* subs = app.add_subcommand("subs", "Write one subtitle cue");
    subs->add_option("--format", format, "srt, vtt, or sbv");
    subs->add_option("--out", out, "Output path")->required();
    subs->add_option("--text", cue_text, "Cue text")->required();
    subs->add_option("--t0", t0, "Start time in milliseconds");
    subs->add_option("--t1", t1, "End time in milliseconds");

    std::string decode_src;
    std::string decode_dst;
    std::uint32_t decode_rate = 16000;
    auto* decode = app.add_subcommand("decode", "Decode a file to mono s16 WAV");
    decode->add_option("--src", decode_src, "Source audio")->required();
    decode->add_option("--dst", decode_dst, "Destination WAV")->required();
    decode->add_option("--rate", decode_rate, "Output sample rate");

    std::string lang_text;
    auto* lang = app.add_subcommand("lang", "Normalize a whisper language token");
    lang->add_option("--text", lang_text, "Language name or code")->required();

    std::string wav_src;
    std::string wav_dst;
    int wav_frames = 0;
    int wav_rate = 16000;
    auto* wav = app.add_subcommand("wav", "Write silence or read a WAV down to 16 kHz");
    wav->add_option("--src", wav_src, "WAV to read");
    wav->add_option("--dst", wav_dst, "WAV to write");
    wav->add_option("--frames", wav_frames, "Silence length when writing");
    wav->add_option("--rate", wav_rate, "Sample rate when writing");

    std::string probe_model;
    auto* probe = app.add_subcommand("probe", "Look up a ggml whisper model");
    probe->add_option("--model", probe_model, "Model id, filename, or path")->required();

    std::string speak_text;
    std::string speak_src;
    auto* speak = app.add_subcommand("speak", "Prepare text for speech");
    speak->add_option("--text", speak_text, "Inline text");
    speak->add_option("--src", speak_src, "Text or markdown file");

    auto* moss = app.add_subcommand("moss", "Check the MOSS Nano loader without a model");
    auto* voice = app.add_subcommand("voice", "Replay one utterance through the voice pipeline");

    std::string record_dst;
    std::string record_session;
    auto* record = app.add_subcommand("record", "Write a short WAV or exercise the record session file");
    record->add_option("--dst", record_dst, "WAV written from a PCM buffer");
    record->add_option("--session", record_session, "Directory for a throwaway session file");

#if defined(PM_AUDIO_SOUND_TRIGGER) && PM_AUDIO_SOUND_TRIGGER
    const SoundTriggerCli sound_trigger = add_sound_trigger(app);
#endif

    auto* devices = app.add_subcommand("devices", "Count capture and playback devices");
    auto* runtimes = app.add_subcommand("runtimes", "Show ONNX and whisper.cpp pointed at Tanit");

    try {
        app.parse(argc, argv);
    } catch (const CLI::ParseError& err) {
        const int code = app.exit(err);
        return code == 0 ? 0 : 2;
    }

    if (selftest->parsed())
        return cmd_selftest(suite);
    if (match->parsed())
        return cmd_match(match_text, wake, wake_off);
    if (subs->parsed())
        return cmd_subs(format, out, cue_text, t0, t1);
    if (decode->parsed())
        return cmd_decode(decode_src, decode_dst, decode_rate);
    if (lang->parsed())
        return cmd_lang(lang_text);
    if (wav->parsed()) {
        if (!wav_dst.empty())
            return cmd_wav_write(wav_dst, wav_frames, wav_rate);
        if (!wav_src.empty())
            return cmd_wav_read(wav_src);
        return fail("EINVAL", "wav needs --src or --dst", 400);
    }
    if (probe->parsed())
        return cmd_probe(probe_model);
    if (speak->parsed()) {
        if (speak_text.empty() && speak_src.empty())
            return fail("EINVAL", "speak needs --text or --src", 400);
        return cmd_speak(speak_text, speak_src);
    }
    if (moss->parsed())
        return cmd_moss();
    if (voice->parsed())
        return cmd_voice();
    if (record->parsed()) {
        if (!record_dst.empty())
            return cmd_record_wav(record_dst);
        if (!record_session.empty())
            return cmd_record_session(record_session);
        return fail("EINVAL", "record needs --dst or --session", 400);
    }
#if defined(PM_AUDIO_SOUND_TRIGGER) && PM_AUDIO_SOUND_TRIGGER
    if (sound_trigger.root && sound_trigger.root->parsed())
        return sound_trigger.run();
#endif
    if (devices->parsed())
        return cmd_devices();
    if (runtimes->parsed())
        return cmd_runtimes();
    return 2;
}
