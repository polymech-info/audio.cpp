#include "pm/audio/vad_gate_selftest.hpp"

#include "pm/audio/vad.hpp"

#include <cmath>
#include <cstdint>
#include <string>
#include <vector>

namespace pm::audio {
namespace {

struct CheckState {
    std::vector<std::pair<std::string, std::string>> failures;
    void expect(bool ok, const char* name, std::string detail = {}) {
        if (!ok) failures.emplace_back(name, std::move(detail));
    }
};

void fill_tone(std::vector<std::int16_t>& out, int rate_hz, int ms, int amp = 12000) {
    const int n = rate_hz * ms / 1000;
    out.resize(static_cast<std::size_t>(n));
    for (int i = 0; i < n; ++i) {
        const double t = static_cast<double>(i) / static_cast<double>(rate_hz);
        out[static_cast<std::size_t>(i)] =
            static_cast<std::int16_t>(amp * std::sin(2.0 * 3.14159265358979323846 * 440.0 * t));
    }
}

void feed_blocks(TranslatePlaybackGate& gate, const std::int16_t* pcm, std::size_t n, int rate_hz) {
    const std::size_t chunk = static_cast<std::size_t>(rate_hz) / 50; // 20 ms
    for (std::size_t off = 0; off < n; off += chunk) {
        const std::size_t take = std::min(chunk, n - off);
        gate.feed_mic(pcm + off, take, rate_hz);
    }
}

void two_turns_when_playback_idle(CheckState& st) {
    constexpr int kRate = 16000;
    std::vector<SpeechEvent> events;
    TranslatePlaybackGate gate;
    std::string err;
    TranslatePlaybackGate::Config cfg;
    cfg.mode = PlaybackMode::conversational;
    cfg.vad.sample_rate_hz = kRate;
    cfg.vad.silence_ms = 500;
    cfg.vad.speech_onset_ms = 60;
    cfg.vad.min_utterance_ms = 200;
    cfg.vad.rms_fallback_threshold = 200.f;
    cfg.translated_rate_hz = 24000;

    const bool opened = gate.open(
        cfg,
        [](const std::int16_t*, std::size_t) {},
        err);
    st.expect(opened, "vad-gate-open", err);
    if (!opened) return;

    gate.set_playback_active([] { return false; });
    gate.set_vad_hook([&events](SpeechEvent e, float, std::size_t) { events.push_back(e); });

    std::vector<std::int16_t> tone;
    fill_tone(tone, kRate, 700);
    std::vector<std::int16_t> quiet(static_cast<std::size_t>(kRate) * 800 / 1000, 0);

    feed_blocks(gate, tone.data(), tone.size(), kRate);
    feed_blocks(gate, quiet.data(), quiet.size(), kRate);
    feed_blocks(gate, tone.data(), tone.size(), kRate);
    feed_blocks(gate, quiet.data(), quiet.size(), kRate);

    int starts = 0;
    int ends = 0;
    int first_end = -1;
    int second_start = -1;
    for (std::size_t i = 0; i < events.size(); ++i) {
        if (events[i] == SpeechEvent::speech_start) {
            ++starts;
            if (starts == 2) second_start = static_cast<int>(i);
        }
        if (events[i] == SpeechEvent::speech_end) {
            ++ends;
            if (ends == 1) first_end = static_cast<int>(i);
        }
    }
    st.expect(starts >= 2, "vad-gate-two-starts", "starts=" + std::to_string(starts));
    st.expect(ends >= 2, "vad-gate-two-ends", "ends=" + std::to_string(ends));
    st.expect(first_end >= 0 && second_start > first_end,
              "vad-gate-second-after-first-end",
              "first_end=" + std::to_string(first_end)
                  + " second_start=" + std::to_string(second_start));
}

void mic_unblocked_when_playback_marked_idle_during_prime_window(CheckState& st) {
    // Simulates queue-aware playback still priming: host reports not active on
    // the speaker yet, so utterance B after a ≤5 s pause must still reach VAD.
    constexpr int kRate = 16000;
    std::vector<SpeechEvent> events;
    TranslatePlaybackGate gate;
    std::string err;
    TranslatePlaybackGate::Config cfg;
    cfg.mode = PlaybackMode::conversational;
    cfg.vad.sample_rate_hz = kRate;
    cfg.vad.silence_ms = 500;
    cfg.vad.speech_onset_ms = 60;
    cfg.vad.min_utterance_ms = 200;
    cfg.vad.rms_fallback_threshold = 200.f;

    const bool opened = gate.open(cfg, [](const std::int16_t*, std::size_t) {}, err);
    st.expect(opened, "vad-gate-prime-open", err);
    if (!opened) return;

    bool speaker_live = false;
    gate.set_playback_active([&speaker_live] { return speaker_live; });
    gate.set_vad_hook([&events](SpeechEvent e, float, std::size_t) { events.push_back(e); });

    std::vector<std::int16_t> tone;
    fill_tone(tone, kRate, 600);
    std::vector<std::int16_t> quiet(static_cast<std::size_t>(kRate) * 700 / 1000, 0);

    feed_blocks(gate, tone.data(), tone.size(), kRate);
    feed_blocks(gate, quiet.data(), quiet.size(), kRate);
    events.clear();

    // Model audio queued but pre-buffer has not opened the speaker (fixed fifo).
    speaker_live = false;
    feed_blocks(gate, tone.data(), tone.size(), kRate);

    int starts = 0;
    for (const auto e : events) {
        if (e == SpeechEvent::speech_start) ++starts;
    }
    st.expect(starts >= 1, "vad-gate-mic-during-prime", "starts=" + std::to_string(starts));
}

} // namespace

VadGateSelfTestReport run_vad_gate_selftest() {
    CheckState st;
    two_turns_when_playback_idle(st);
    mic_unblocked_when_playback_marked_idle_during_prime_window(st);
    VadGateSelfTestReport out;
    out.ok = st.failures.empty();
    if (!out.ok) {
        out.detail = st.failures.front().first;
        if (!st.failures.front().second.empty())
            out.detail += ": " + st.failures.front().second;
    }
    return out;
}

} // namespace pm::audio