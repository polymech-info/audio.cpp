#include "pm/audio/session/audio_session_selftest.hpp"

#include "pm/audio/session/audio_events.hpp"
#include "pm/audio/session/audio_graph.hpp"
#include "pm/audio/session/audio_lease.hpp"
#include "pm/audio/session/audio_route.hpp"

#include <atomic>
#include <cstring>
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
        K::WalkieTransmit, K::GroupVoice, K::LanRemote, K::PhoneCall,
    };
    const K incoming[] = {
        K::None, K::Dictation, K::ReadAloud, K::AiConversation, K::WalkieListen,
        K::WalkieTransmit, K::GroupVoice, K::LanRemote, K::PhoneCall,
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
    if (suite != "all" && suite != "lease" && suite != "route" && suite != "event" && suite != "graph") {
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
    report.ok = st.failures.empty();
    report.json = report_json(suite, st);
    return report;
}

} // namespace pm::audio::session
