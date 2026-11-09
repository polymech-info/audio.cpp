#include "pm/audio/session/audio_session_selftest.hpp"

#include "pm/audio/pcm_record.hpp"
#include "pm/audio/ring.hpp"
#include "pm/audio/session/audio_events.hpp"
#include "pm/audio/session/audio_graph.hpp"
#include "pm/audio/session/audio_lease.hpp"
#include "pm/audio/session/audio_route.hpp"
#include "pm/audio/voice/voice_pipeline.hpp"

#include <atomic>
#include <cstdint>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <limits>
#include <memory>
#include <string>
#include <thread>
#include <utility>
#include <vector>

namespace pm::audio::session {
namespace {

struct State {
    int checks = 0;
    std::vector<std::pair<std::string, std::string>> failures;

    void expect(bool cond, const char* name, const std::string& detail = {})
    {
        ++checks;
        if (!cond)
            failures.emplace_back(name, detail);
    }
};

bool want_suite(std::string_view suite, const char* part)
{
    return suite == "all" || suite == part;
}

void check_lease(State& st)
{
    using K = SessionKind;
    const K holders[] = {
        K::None, K::Dictation, K::ReadAloud, K::AiConversation, K::WalkieListen,
        K::WalkieTransmit, K::GroupVoice, K::LanRemote, K::PhoneCall, K::Recorder,
    };
    const K incoming[] = {
        K::None, K::Dictation, K::ReadAloud, K::AiConversation, K::WalkieListen,
        K::WalkieTransmit, K::GroupVoice, K::LanRemote, K::PhoneCall, K::Recorder,
    };
    for (K h : holders) {
        for (K in : incoming) {
            const LeaseDecision d = preempt(h, in);
            const std::string name = std::string("lease reason ")
                + session_kind_name(h) + " <- " + session_kind_name(in);
            st.expect(d.reason != nullptr && d.reason[0] != '\0', name.c_str());
        }
    }

    struct Pin {
        K holder;
        K incoming;
        LeaseAction action;
        const char* reason;
        const char* name;
    };
    const Pin pins[] = {
        {K::None, K::Dictation, LeaseAction::Allow, "idle", "idle allows dictation"},
        {K::None, K::PhoneCall, LeaseAction::Allow, "idle", "idle allows call notice"},
        {K::None, K::Recorder, LeaseAction::Allow, "idle", "idle allows recording"},
        {K::Dictation, K::Recorder, LeaseAction::Preempt, "recording replaces dictation", "recording replaces dictation"},
        {K::Recorder, K::Dictation, LeaseAction::Deny, "audio lease busy", "dictation waits out a recording"},
        {K::Dictation, K::AiConversation, LeaseAction::Preempt, "ai replaces dictation", "ai replaces dictation"},
        {K::AiConversation, K::Dictation, LeaseAction::Deny, "audio lease busy", "dictation does not replace ai"},
        {K::Dictation, K::WalkieTransmit, LeaseAction::Deny, "audio lease busy", "walkie grant waits out dictation"},
        {K::Dictation, K::ReadAloud, LeaseAction::Deny, "audio lease busy", "read-aloud waits out dictation"},
        {K::ReadAloud, K::Dictation, LeaseAction::Preempt, "replaces read-aloud", "dictation replaces read-aloud"},
        {K::Dictation, K::PhoneCall, LeaseAction::Preempt, "call", "call preempts dictation"},
        {K::AiConversation, K::PhoneCall, LeaseAction::Preempt, "call", "call preempts ai"},
        {K::WalkieTransmit, K::PhoneCall, LeaseAction::Preempt, "call", "call preempts walkie"},
        {K::GroupVoice, K::PhoneCall, LeaseAction::Preempt, "call", "call preempts group"},
        {K::PhoneCall, K::Dictation, LeaseAction::Deny, "call in progress", "call blocks dictation"},
        {K::WalkieListen, K::WalkieTransmit, LeaseAction::Allow, "transmit upgrade", "walkie transmit upgrade"},
        {K::WalkieTransmit, K::WalkieListen, LeaseAction::Allow, "return to listening", "walkie return to listening"},
        {K::GroupVoice, K::AiConversation, LeaseAction::Deny, "group audio does not enter the voice agent", "group does not enter voice agent"},
        {K::AiConversation, K::LanRemote, LeaseAction::Allow, "no device", "lan remote beside ai"},
        {K::LanRemote, K::AiConversation, LeaseAction::Allow, "lan holds no device", "ai while lan remote"},
        {K::Dictation, K::Dictation, LeaseAction::Deny, "already active", "dictation already active"},
        {K::None, K::None, LeaseAction::Deny, "no session", "empty request denied"},
    };
    for (const Pin& pin : pins) {
        const LeaseDecision d = preempt(pin.holder, pin.incoming);
        const bool ok = d.action == pin.action && std::strcmp(d.reason, pin.reason) == 0;
        std::string detail;
        if (!ok) {
            detail = std::string(lease_action_name(d.action)) + " " + d.reason;
        }
        st.expect(ok, pin.name, detail);
    }

    st.expect(!occupies_audio_lease(K::None), "none holds no lease");
    st.expect(!occupies_audio_lease(K::LanRemote), "lan remote holds no lease");
    st.expect(occupies_audio_lease(K::Dictation), "dictation holds a lease");
    st.expect(occupies_audio_lease(K::WalkieListen), "walkie listen holds a lease");
    st.expect(std::strcmp(session_kind_name(K::AiConversation), "ai") == 0, "ai kind name");
    st.expect(parse_session_kind("walkie_transmit") == K::WalkieTransmit, "parse walkie transmit");
    st.expect(parse_session_kind("nope") == K::None, "unknown kind is none");

    lease_force_clear();
    const LeaseGrant dictation = lease_acquire(K::Dictation);
    st.expect(dictation.ok && dictation.token != 0 && lease_holder() == K::Dictation, "owner grants dictation");
    const LeaseGrant again = lease_acquire(K::Dictation);
    st.expect(!again.ok && std::strcmp(again.reason, "already active") == 0, "owner refuses a second dictation");
    const LeaseGrant ai = lease_acquire(K::AiConversation);
    st.expect(ai.ok && ai.action == LeaseAction::Preempt && ai.token != dictation.token, "owner preempts dictation for ai");
    st.expect(!lease_release(dictation.token), "preempted token cannot release");
    st.expect(!lease_allows_capture(K::ReadAloud) && lease_allows_playback(K::ReadAloud), "read-aloud is playback only");
    st.expect(lease_allows_capture(K::Dictation) && !lease_allows_playback(K::Dictation), "dictation is capture only");
    st.expect(lease_release(ai.token) && lease_holder() == K::None, "owner release clears the holder");
    st.expect(!lease_release(ai.token), "released token cannot release again");

    const LeaseGrant call = lease_acquire(K::PhoneCall);
    st.expect(call.ok && call.token != 0 && lease_holder() == K::PhoneCall, "idle call takes the lease");
    const LeaseGrant during = lease_acquire(K::Dictation);
    st.expect(!during.ok && std::strcmp(during.reason, "call in progress") == 0, "call blocks a new session");
    st.expect(lease_release(call.token) && lease_holder() == K::None, "call release clears the holder");
    st.expect(!lease_release(call.token), "call token cannot release twice");
    const LeaseGrant lan = lease_acquire(K::LanRemote);
    st.expect(lan.ok && lan.token == 0 && lease_holder() == K::None, "lan remote still takes no lease");
}

void check_route(State& st)
{
    const DeviceSelector def = parse_device_selector("  ");
    st.expect(def.ok && def.is_default, "blank selector is default");

    const DeviceSelector name = parse_device_selector(" Jabra Elite ");
    st.expect(name.ok && name.kind == SelectorKind::Name && name.name == "Jabra Elite", "name selector");

    const DeviceSelector id = parse_device_selector("aaudio:42");
    st.expect(id.ok && id.kind == SelectorKind::AaudioId && id.aaudio_id == 42, "aaudio id");

    const DeviceSelector bad = parse_device_selector("aaudio:");
    st.expect(!bad.ok, "empty aaudio id rejected");
    const DeviceSelector neg = parse_device_selector("aaudio:-1");
    st.expect(!neg.ok, "negative aaudio id rejected");
    const DeviceSelector mix = parse_device_selector("aaudio:12x");
    st.expect(!mix.ok, "trailing junk rejected");
    const DeviceSelector titled = parse_device_selector("AAudio:3");
    st.expect(titled.ok && titled.kind == SelectorKind::Name, "prefix is case sensitive");

    const RouteCandidate devices[] = {
        {7, "bluetooth", "Jabra Elite", "aa:bb", true},
        {8, "bluetooth", "Jabra Elite", "cc:dd", true},
        {3, "wired", "Headset", "", true},
        {11, "builtin", "Pixel Mic", "", false},
    };

    const RouteMatch by_addr = match_descriptor({"", "", "aa:bb"}, devices, 4);
    st.expect(by_addr.ok && by_addr.id == 7, "address match");

    const RouteMatch addr_miss = match_descriptor({"bluetooth", "Other", "zz"}, devices, 4);
    st.expect(!addr_miss.ok, "address miss does not fall through");

    const RouteMatch ambiguous = match_descriptor({"bluetooth", "Jabra Elite", ""}, devices, 4);
    st.expect(!ambiguous.ok && std::strcmp(ambiguous.reason, "ambiguous device") == 0, "ambiguous product");

    const RouteMatch wired = match_descriptor({"wired", "headset", ""}, devices, 4);
    st.expect(wired.ok && wired.id == 3, "type and product match");

    const RouteMatch only_type = match_descriptor({"wired", "", ""}, devices, 4);
    st.expect(only_type.ok && only_type.id == 3, "unique type match");

    const RouteMatch empty = match_descriptor({}, devices, 4);
    st.expect(!empty.ok && std::strcmp(empty.reason, "empty device descriptor") == 0, "empty descriptor");

    const OpenVerdict match = verify_opened_device(true, 7, 7);
    st.expect(match.transmit, "matching open may transmit");
    const OpenVerdict differ = verify_opened_device(true, 7, 3);
    st.expect(!differ.transmit && std::strcmp(differ.reason, "opened device differs from the request") == 0,
              "different open must not transmit");
    const OpenVerdict unread = verify_opened_device(true, 7, -1);
    st.expect(!unread.transmit, "unreadable open must not transmit");
    const OpenVerdict fallback = verify_opened_device(false, -1, 3);
    st.expect(fallback.transmit, "default open may transmit");

    const RouteLoss specific = on_route_lost(true);
    const RouteLoss generic = on_route_lost(false);
    st.expect(specific.action == RouteLossAction::Stop && generic.action == RouteLossAction::Stop,
              "route loss always stops");

    const CommChoice out = select_communication_output(7, devices, 4);
    st.expect(out.ok && out.output_id == 7, "communication output accepted");
    const CommChoice mic = select_communication_output(11, devices, 4);
    st.expect(!mic.ok && std::strstr(mic.reason, "output device") != nullptr, "microphone id rejected");
    const CommChoice missing = select_communication_output(99, devices, 4);
    st.expect(!missing.ok, "unknown communication id rejected");
}

void check_event(State& st)
{
    st.expect(std::strcmp(audio_method_name(AudioMethod::TextDelta), "realtimeTextDelta") == 0, "text delta name");
    st.expect(std::strcmp(audio_method_name(AudioMethod::ConsentRequest), "consentRequest") == 0, "consent name");
    st.expect(std::strcmp(audio_method_name(AudioMethod::ConsentDismiss), "dismissConsent") == 0, "dismiss name");
    st.expect(std::strcmp(audio_method_name(AudioMethod::AskUser), "askUser") == 0, "ask name");
    st.expect(std::strcmp(audio_method_name(AudioMethod::Done), "realtimeDone") == 0, "done name");
    st.expect(std::strcmp(audio_method_name(AudioMethod::DeviceLost), "deviceLost") == 0, "device lost name");

    AudioEnvelope ev;
    ev.session_id = "s\"1";
    ev.kind = SessionKind::AiConversation;
    ev.seq = 4;
    ev.ts_ms = 9;
    ev.participant_id = "p";
    ev.method = AudioMethod::TextDelta;
    ev.args_json = "[\"hi\"]";
    const std::string json = envelope_json(ev);
    st.expect(json.find("\"session_id\":\"s\\\"1\"") != std::string::npos, "session id escaped");
    st.expect(json.find("\"session_kind\":\"ai\"") != std::string::npos, "kind in envelope");
    st.expect(json.find("\"method\":\"realtimeTextDelta\"") != std::string::npos, "method in envelope");
    st.expect(json.find("\"seq\":4") != std::string::npos, "seq in envelope");
    st.expect(json.find("\"args\":[\"hi\"]") != std::string::npos, "args embedded");

    ev.args_json = "not json";
    const std::string fallback = envelope_json(ev);
    st.expect(fallback.find("\"args\":[]") != std::string::npos, "bad args become empty array");
}

void check_graph(State& st)
{
    using pm::audio::graph::ConnectResult;
    using pm::audio::graph::FrameQueue;
    using pm::audio::graph::PcmFormat;
    using pm::audio::graph::Pressure;
    using pm::audio::graph::ReplaySource;
    using pm::audio::graph::SpscRing;
    using pm::audio::graph::Tee;
    using pm::audio::graph::apply_gain;
    using pm::audio::graph::connect_formats;

    const PcmFormat pcm16{16000, 1};
    const ConnectResult good = connect_formats(pcm16, pcm16);
    st.expect(good.ok, "matching formats connect");
    const ConnectResult bad = connect_formats(pcm16, PcmFormat{48000, 1});
    st.expect(!bad.ok && std::strcmp(bad.reason, "format mismatch") == 0, "rate mismatch rejected");
    const ConnectResult unset = connect_formats({}, pcm16);
    st.expect(!unset.ok, "unset format rejected");

    SpscRing ring(8);
    st.expect(ring.capacity() >= 8, "ring holds the requested capacity");
    const std::int16_t first[] = {1, 2, 3, 4, 5};
    st.expect(ring.try_write(first, 5) == 5, "ring write");
    std::int16_t got[8] = {};
    st.expect(ring.try_read(got, 5) == 5 && got[0] == 1 && got[4] == 5, "ring read");
    const std::int16_t second[] = {6, 7, 8, 9, 10, 11};
    st.expect(ring.try_write(second, 6) == 6, "ring write after wrap");
    st.expect(ring.try_read(got, 6) == 6 && got[0] == 6 && got[5] == 11, "ring read after wrap");

    std::vector<std::int16_t> fill(ring.capacity(), 1);
    st.expect(ring.try_write(fill.data(), fill.size()) == fill.size(), "ring fills");
    const std::int16_t extra[] = {9, 9, 9};
    st.expect(ring.write_or_drop(extra, 3) == 0 && ring.dropped() == 3, "full ring drops incoming");

    FrameQueue dropping(4, Pressure::DropOldest);
    const std::int16_t ten[] = {1, 2, 3, 4, 5, 6, 7, 8, 9, 10};
    st.expect(dropping.push(ten, 10), "drop-oldest accepts");
    st.expect(dropping.size() == 4 && dropping.dropped() == 6, "drop-oldest keeps the tail");
    std::int16_t tail[4] = {};
    st.expect(dropping.pop(tail, 4) == 4 && tail[0] == 7 && tail[3] == 10, "tail is the newest");

    FrameQueue blocking(4, Pressure::Block);
    st.expect(blocking.push(ten, 4), "block accepts a fitting chunk");
    st.expect(!blocking.push(ten, 1) && blocking.size() == 4 && blocking.dropped() == 0, "block refuses overflow");

    Tee tee;
    tee.add_sink(4, Pressure::DropOldest);
    tee.add_sink(4, Pressure::Block);
    const auto pushed = tee.push(ten, 10);
    st.expect(!pushed.all_accepted && pushed.blocked == 1, "tee isolates a blocked sink");
    st.expect(tee.sink(0).size() == 4 && tee.sink(0).dropped() == 6, "tee drop sink kept the tail");
    st.expect(tee.sink(1).size() == 0, "tee block sink took nothing");

    ReplaySource replay({1, 2, 3, 4, 5, 6, 7, 8, 9, 10}, pcm16, 4);
    std::vector<std::int16_t> hop;
    st.expect(replay.pull(hop) && hop.size() == 4 && hop[0] == 1, "replay first hop");
    st.expect(replay.pull(hop) && hop.size() == 4, "replay second hop");
    st.expect(replay.pull(hop) && hop.size() == 2 && hop[1] == 10, "replay short tail");
    st.expect(!replay.pull(hop) && hop.empty(), "replay exhausted");

    std::vector<std::int16_t> gained;
    const std::int16_t loud[] = {20000, -20000};
    apply_gain(loud, 2, 2.f, gained);
    st.expect(gained.size() == 2 && gained[0] == 32767 && gained[1] == -32768, "gain saturates");

    SpscRing shared(8192);
    constexpr int kCount = 10000;
    std::atomic<int> mismatches{0};
    std::thread consumer([&] {
        int seen = 0;
        std::int16_t buf[128];
        while (seen < kCount) {
            const std::size_t n = shared.try_read(buf, 128);
            for (std::size_t i = 0; i < n; ++i) {
                if (buf[i] != static_cast<std::int16_t>(seen))
                    mismatches.fetch_add(1);
                ++seen;
            }
            if (n == 0)
                std::this_thread::yield();
        }
    });
    for (int i = 0; i < kCount;) {
        std::int16_t chunk[64];
        const int n = std::min(64, kCount - i);
        for (int k = 0; k < n; ++k)
            chunk[k] = static_cast<std::int16_t>(i + k);
        const std::size_t wrote = shared.try_write(chunk, static_cast<std::size_t>(n));
        i += static_cast<int>(wrote);
        if (wrote == 0)
            std::this_thread::yield();
    }
    consumer.join();
    st.expect(mismatches.load() == 0 && shared.dropped() == 0, "spsc ring under two threads");

    auto bytes = std::make_unique<pm::audio::AudioRingBuffer>();
    const uint8_t sample[] = {1, 2, 3, 4, 5};
    st.expect(bytes->try_push(sample, 5) == 5, "byte ring try_push");
    uint8_t got_bytes[5] = {};
    st.expect(bytes->pop(got_bytes, 5) == 5 && got_bytes[0] == 1 && got_bytes[4] == 5, "byte ring pop");
    st.expect(bytes->try_push(sample, 5) == 5, "byte ring try_push after space opens");
    bytes->cancel();
    st.expect(bytes->try_push(sample, 5) == 0, "cancelled byte ring accepts nothing");
}

bool read_wav_pcm(const std::filesystem::path& path, std::vector<int16_t>& samples, uint16_t& channels)
{
    std::ifstream in(path, std::ios::binary);
    char hdr[44];
    in.read(hdr, 44);
    if (in.gcount() != 44)
        return false;
    channels = static_cast<uint16_t>(static_cast<unsigned char>(hdr[22])
        | (static_cast<unsigned char>(hdr[23]) << 8));
    const uint32_t data_bytes = static_cast<uint32_t>(static_cast<unsigned char>(hdr[40]))
        | (static_cast<uint32_t>(static_cast<unsigned char>(hdr[41])) << 8)
        | (static_cast<uint32_t>(static_cast<unsigned char>(hdr[42])) << 16)
        | (static_cast<uint32_t>(static_cast<unsigned char>(hdr[43])) << 24);
    if (data_bytes % sizeof(int16_t) != 0)
        return false;
    samples.resize(data_bytes / sizeof(int16_t));
    if (!samples.empty())
        in.read(reinterpret_cast<char*>(samples.data()), static_cast<std::streamsize>(data_bytes));
    return static_cast<bool>(in) && in.gcount() == static_cast<std::streamsize>(data_bytes);
}

void check_record(State& st)
{
    st.expect(!pm::audio::pcm_wav_header_fits(0, 48000, 2), "empty wav does not fit");
    st.expect(pm::audio::pcm_wav_header_fits(100, 48000, 2), "short wav fits");
    st.expect(!pm::audio::pcm_wav_header_fits(1ull << 31, 48000, 2), "four gigabyte wav does not fit");
    st.expect(pm::audio::spill_sample_threshold(2, 0x80000000u, 2) == std::numeric_limits<uint32_t>::max(),
              "spill threshold does not wrap");

    pm::audio::PcmRecordBuffer overflow;
    overflow.set_format(16000, 2);
    const int16_t one = 1;
    overflow.push(&one, (std::numeric_limits<size_t>::max() / 2) + 1);
    st.expect(overflow.frame_count() == 0, "overflowing push stores nothing");
    overflow.reserve_seconds(std::numeric_limits<uint32_t>::max());

    std::vector<int16_t> full(pm::audio::PcmRecordBuffer::k_ingress_samples + 8, 4);
    pm::audio::PcmRecordBuffer bounded;
    bounded.set_format(16000, 1);
    bounded.push(full.data(), full.size());
    st.expect(bounded.dropped_samples() == 8, "ingress drops the tail of a huge push");

    pm::audio::PcmRecordBuffer buffer;
    buffer.set_format(8000, 1);
    buffer.set_spill_threshold_seconds(1);
    std::vector<int16_t> tone(8100, 3);
    buffer.push(tone.data(), tone.size());
    st.expect(buffer.sample_rate() == 8000 && buffer.channels() == 1, "format sticks");
    buffer.set_format(16000, 2);
    st.expect(buffer.sample_rate() == 8000 && buffer.channels() == 1, "format change during a recording is ignored");

    const auto dir = std::filesystem::temp_directory_path() / "pm-audio-selftest-record";
    std::error_code ec;
    std::filesystem::remove_all(dir, ec);
    std::filesystem::create_directories(dir, ec);
    const auto good = dir / "clip.wav";
    const auto again = dir / "again.wav";
    st.expect(buffer.write_wav(good), "spill wav writes");
    uint16_t ch = 0;
    std::vector<int16_t> decoded;
    st.expect(read_wav_pcm(good, decoded, ch) && ch == 1 && decoded.size() == tone.size() && decoded.front() == 3 && decoded.back() == 3,
              "spill wav keeps every sample");
    st.expect(buffer.write_wav(again), "second export still has the spill");
    std::vector<int16_t> decoded_again;
    st.expect(read_wav_pcm(again, decoded_again, ch) && decoded_again.size() == tone.size(),
              "failed-free second export matches");

    const auto blocker = dir / "not-a-directory";
    {
        std::ofstream block(blocker, std::ios::binary);
        block << "x";
    }
    st.expect(!buffer.write_wav(blocker / "clip.wav"), "wav export fails when the parent is a file");
    st.expect(buffer.frame_count() == tone.size(), "failed export keeps the recording");
    const auto recovered = dir / "recovered.wav";
    st.expect(buffer.write_wav(recovered), "recording exports after a failed write");
    std::vector<int16_t> recovered_pcm;
    st.expect(read_wav_pcm(recovered, recovered_pcm, ch) && recovered_pcm.size() == tone.size(),
              "recovered wav matches");

    pm::audio::PcmRecordBuffer live;
    live.set_format(16000, 1);
    std::vector<int16_t> first(1000, 1);
    live.push(first.data(), first.size());
    const auto snap = dir / "snap.wav";
    const auto later = dir / "later.wav";
    std::atomic<bool> started{false};
    std::thread writer([&] {
        started.store(true);
        live.write_wav(snap);
    });
    while (!started.load())
        std::this_thread::yield();
    std::vector<int16_t> extra(1000, 2);
    live.push(extra.data(), extra.size());
    writer.join();
    st.expect(live.write_wav(later), "export after a concurrent push");
    std::vector<int16_t> snap_pcm;
    std::vector<int16_t> later_pcm;
    const bool snap_ok = read_wav_pcm(snap, snap_pcm, ch);
    const bool later_ok = read_wav_pcm(later, later_pcm, ch);
    st.expect(snap_ok && (snap_pcm.size() == 1000 || snap_pcm.size() == 2000), "snapshot length is the prefix");
    st.expect(later_ok && later_pcm.size() == 2000 && later_pcm[0] == 1 && later_pcm.back() == 2,
              "concurrent push is still in the buffer");
    std::filesystem::remove_all(dir, ec);
}

void check_voice(State& st)
{
    using pm::audio::voice::PipelineConfig;
    using pm::audio::voice::VoicePipeline;
    PipelineConfig bad_cfg;
    bad_cfg.capture_format.sample_rate = 1000;
    bad_cfg.capture_format.channels = 1;
    VoicePipeline bad(bad_cfg, {});
    std::string error;
    st.expect(!bad.start_replay(error), "voice rejects a format before joining a worker");

    PipelineConfig cfg;
    cfg.capture_format.sample_rate = 16000;
    cfg.capture_format.channels = 1;
    cfg.callback_queue_ms = 20;
    VoicePipeline pipeline(cfg, {});
    st.expect(pipeline.start_replay(error), "voice worker starts");
    std::vector<int16_t> chunk(8000, 0);
    st.expect(!pipeline.inject_pcm(chunk.data(), chunk.size()), "oversized callback chunk is refused");
    const auto stats = pipeline.stats();
    st.expect(stats.dropped_overflow == chunk.size() && stats.dropped_lock == 0,
              "overflow is counted apart from lock contention");
    st.expect(!pipeline.start_replay(error), "a running voice worker refuses a second start");
    pipeline.stop();
}

std::string json_escape(std::string_view s)
{
    std::string out;
    for (unsigned char c : s) {
        if (c == '"' || c == '\\')
            out.push_back('\\');
        out.push_back(static_cast<char>(c));
    }
    return out;
}

std::string report_json(std::string_view suite, const State& st)
{
    std::string out = "{\"ok\":";
    out += st.failures.empty() ? "true" : "false";
    out += ",\"function\":\"audio-session\",\"suite\":\"";
    out.append(suite);
    out += "\",\"checks\":";
    out += std::to_string(st.checks);
    out += ",\"failed\":";
    out += std::to_string(st.failures.size());
    out += ",\"failures\":[";
    for (std::size_t i = 0; i < st.failures.size(); ++i) {
        if (i)
            out += ',';
        out += "{\"name\":\"";
        out += json_escape(st.failures[i].first);
        out += "\",\"detail\":\"";
        out += json_escape(st.failures[i].second);
        out += "\"}";
    }
    out += "]}";
    return out;
}

} // namespace

SelfTestReport run_selftest(std::string_view suite)
{
    SelfTestReport report;
    if (suite != "all" && suite != "lease" && suite != "route" && suite != "event" && suite != "graph"
        && suite != "record" && suite != "voice") {
        report.json = "{\"ok\":false,\"function\":\"audio-session\",\"error\":\"unknown suite\"}";
        return report;
    }
    State st;
    if (want_suite(suite, "lease"))
        check_lease(st);
    if (want_suite(suite, "route"))
        check_route(st);
    if (want_suite(suite, "event"))
        check_event(st);
    if (want_suite(suite, "graph"))
        check_graph(st);
    if (want_suite(suite, "record"))
        check_record(st);
    if (want_suite(suite, "voice"))
        check_voice(st);
    report.ok = st.failures.empty();
    report.json = report_json(suite, st);
    return report;
}

} // namespace pm::audio::session
