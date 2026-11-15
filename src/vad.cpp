#include "pm/audio/log.hpp"
#include "pm/audio/vad.hpp"

#include <algorithm>
#include <atomic>
#include <cmath>
#include <cstring>
#include <mutex>
#include <vector>

#if defined(HAVE_WEBRTC_VAD)
#include "common_audio/vad/include/webrtc_vad.h"
#endif

namespace pm::audio {
namespace {

int samples_per_frame(int rate_hz, int frame_ms) {
    return (rate_hz * frame_ms) / 1000;
}

float rms_i16(const std::int16_t* pcm, std::size_t n) {
    if (!pcm || n == 0) return 0.f;
    double acc = 0.0;
    for (std::size_t i = 0; i < n; ++i) {
        const double v = static_cast<double>(pcm[i]);
        acc += v * v;
    }
    return static_cast<float>(std::sqrt(acc / static_cast<double>(n)));
}

// Naive downsample for VAD (mic may be 24 kHz, WebRTC wants 16 kHz).
void downsample_to(const std::int16_t* in, std::size_t in_n, int in_rate,
                   std::vector<std::int16_t>& out, int out_rate) {
    out.clear();
    if (!in || in_n == 0 || in_rate <= 0 || out_rate <= 0) return;
    if (in_rate == out_rate) {
        out.assign(in, in + in_n);
        return;
    }
    const double step = static_cast<double>(in_rate) / static_cast<double>(out_rate);
    const std::size_t out_n = static_cast<std::size_t>(static_cast<double>(in_n) / step);
    out.resize(out_n);
    for (std::size_t i = 0; i < out_n; ++i) {
        const std::size_t src = static_cast<std::size_t>(static_cast<double>(i) * step);
        out[i] = in[std::min(src, in_n - 1)];
    }
}

} // namespace

// ---------------------------------------------------------------------------
// SpeechActivity
// ---------------------------------------------------------------------------

struct SpeechActivity::Impl {
    VadFrameConfig cfg{};
    Listener listener;
    bool opened = false;
    bool talking = false;
    int onset_ms = 0;
    int silence_ms = 0;
    int utter_ms = 0;
    std::vector<std::int16_t> pending;
    std::string backend = "rms";
#if defined(HAVE_WEBRTC_VAD)
    VadInst* vad = nullptr;
#endif

    void emit(SpeechEvent e, float level) {
        if (listener) listener(e, level);
    }

    bool classify_frame(const std::int16_t* frame, int n, float& level) {
        level = rms_i16(frame, static_cast<std::size_t>(n));
        // Reject near-silent "speech" — WebRTC can stick true on echo tails / zeros.
        if (level < cfg.rms_fallback_threshold * 0.35f)
            return false;
#if defined(HAVE_WEBRTC_VAD)
        if (vad) {
            const int r = WebRtcVad_Process(vad, cfg.sample_rate_hz, frame, n);
            return r == 1;
        }
#endif
        return level >= cfg.rms_fallback_threshold;
    }

    void reset_turn() {
        talking = false;
        onset_ms = 0;
        silence_ms = 0;
        utter_ms = 0;
        pending.clear();
    }

    void advance(bool speech, float level) {
        const int fm = cfg.frame_ms;
        if (!talking) {
            if (speech) {
                onset_ms += fm;
                if (onset_ms >= cfg.speech_onset_ms) {
                    talking = true;
                    silence_ms = 0;
                    utter_ms = onset_ms;
                    onset_ms = 0;
                    emit(SpeechEvent::speech_start, level);
                }
            } else {
                onset_ms = 0;
            }
            return;
        }

        utter_ms += fm;
        if (speech) {
            silence_ms = 0;
            emit(SpeechEvent::activity, level);
        } else {
            silence_ms += fm;
            if (silence_ms >= cfg.silence_ms && utter_ms >= cfg.min_utterance_ms) {
                talking = false;
                silence_ms = 0;
                utter_ms = 0;
                emit(SpeechEvent::speech_end, level);
            } else if (utter_ms >= cfg.max_segment_ms) {
                talking = false;
                silence_ms = 0;
                utter_ms = 0;
                emit(SpeechEvent::speech_end, level);
            }
        }
    }
};

SpeechActivity::SpeechActivity() : m_(std::make_unique<Impl>()) {}
SpeechActivity::~SpeechActivity() { close(); }

bool SpeechActivity::open(const VadFrameConfig& cfg, std::string& err) {
    close();
    m_->cfg = cfg;
    if (m_->cfg.silence_ms < 200) m_->cfg.silence_ms = 200;
    if (m_->cfg.silence_ms > 2000) m_->cfg.silence_ms = 2000;
    if (m_->cfg.frame_ms != 10 && m_->cfg.frame_ms != 20 && m_->cfg.frame_ms != 30)
        m_->cfg.frame_ms = 20;
    if (m_->cfg.sample_rate_hz != 8000 && m_->cfg.sample_rate_hz != 16000 &&
        m_->cfg.sample_rate_hz != 32000 && m_->cfg.sample_rate_hz != 48000)
        m_->cfg.sample_rate_hz = 16000;

#if defined(HAVE_WEBRTC_VAD)
    m_->vad = WebRtcVad_Create();
    if (!m_->vad) {
        err = "WebRtcVad_Create failed";
        return false;
    }
    if (WebRtcVad_Init(m_->vad) != 0) {
        WebRtcVad_Free(m_->vad);
        m_->vad = nullptr;
        err = "WebRtcVad_Init failed";
        return false;
    }
    WebRtcVad_set_mode(m_->vad, std::clamp(m_->cfg.mode, 0, 3));
    m_->backend = "webrtc";
#else
    m_->backend = "rms";
    (void)err;
#endif
    m_->opened = true;
    return true;
}

void SpeechActivity::close() {
#if defined(HAVE_WEBRTC_VAD)
    if (m_->vad) {
        WebRtcVad_Free(m_->vad);
        m_->vad = nullptr;
    }
#endif
    m_->opened = false;
    m_->talking = false;
    m_->onset_ms = 0;
    m_->silence_ms = 0;
    m_->utter_ms = 0;
    m_->pending.clear();
}

bool SpeechActivity::ok() const { return m_ && m_->opened; }

bool SpeechActivity::feed(const std::int16_t* pcm, std::size_t frames) {
    if (!ok() || !pcm || frames == 0) return m_->talking;
    const int spf = samples_per_frame(m_->cfg.sample_rate_hz, m_->cfg.frame_ms);
    if (spf <= 0) return m_->talking;

    m_->pending.insert(m_->pending.end(), pcm, pcm + frames);
    while (static_cast<int>(m_->pending.size()) >= spf) {
        // Copy the frame out first. speech_end resets the turn and clears
        // pending from inside the listener — erasing after that is a crash.
        std::vector<std::int16_t> frame(m_->pending.begin(),
                                        m_->pending.begin() + spf);
        m_->pending.erase(m_->pending.begin(), m_->pending.begin() + spf);
        float level = 0.f;
        const bool speech = m_->classify_frame(frame.data(), spf, level);
        m_->advance(speech, level);
    }
    return m_->talking;
}

bool SpeechActivity::in_speech() const { return m_ && m_->talking; }
void SpeechActivity::reset_turn() { if (m_) m_->reset_turn(); }
const char* SpeechActivity::backend() const { return m_ ? m_->backend.c_str() : "none"; }
void SpeechActivity::set_listener(Listener cb) { if (m_) m_->listener = std::move(cb); }

// ---------------------------------------------------------------------------
// TranslatePlaybackGate
// ---------------------------------------------------------------------------

struct TranslatePlaybackGate::Impl {
    Config cfg{};
    Sink sink;
    VadHook vad_hook;
    PlaybackActive playback_active;
    SpeechActivity vad;
    std::mutex mu;
    std::vector<std::int16_t> held;
    // Conversational starts holding so early model audio cannot leak mid-utterance.
    std::atomic<bool> holding{true};
    std::atomic<bool> opened{false};
    bool mute_logged = false;
    std::vector<std::int16_t> mic_tmp;

    void play_held_locked(const char* why) {
        if (held.empty()) {
            log::debug(std::string("translate gate: ") + why + " held=0");
            return;
        }
        const std::size_t n = held.size();
        if (sink) sink(held.data(), n);
        held.clear();
        mute_logged = true;
        const int rate = std::max(1, cfg.translated_rate_hz);
        const int ms = static_cast<int>((n * 1000ull) / static_cast<unsigned long long>(rate));
        log::debug(std::string("translate gate: ") + why
                   + " frames=" + std::to_string(n)
                   + " audio_ms=" + std::to_string(ms)
                   + " uplink=muted");
    }

    void append_held_locked(const std::int16_t* pcm, std::size_t frames) {
        const std::size_t cap = m_cap();
        m_held_insert(pcm, frames, cap);
    }

    std::size_t m_cap() const {
        return cfg.max_buffered_frames > 0 ? cfg.max_buffered_frames : static_cast<std::size_t>(24000 * 60);
    }

    void m_held_insert(const std::int16_t* pcm, std::size_t frames, std::size_t cap) {
        held.insert(held.end(), pcm, pcm + frames);
        if (cap > 0 && held.size() > cap) {
            held.erase(held.begin(), held.begin() + static_cast<std::ptrdiff_t>(held.size() - cap));
        }
    }

    bool playback_is_active() const {
        return playback_active && playback_active();
    }

    bool playout_blocks_mic() const {
        return playback_is_active();
    }

    void on_vad(SpeechEvent e, float level) {
        std::size_t held_n = 0;
        {
            std::lock_guard<std::mutex> lk(mu);
            // Render/queue state is authoritative. This prevents speaker echo
            // from re-closing the gate while a streamed sentence is playing.
            // Ignore echo onsets during playout, but never strand a real local
            // turn that began during the jitter lead. Its AEC-cleaned tail must
            // reach VAD so speech_end can close and release that turn.
            const bool finish_active_turn =
                e == SpeechEvent::speech_end && holding.load();
            if (playout_blocks_mic() && !finish_active_turn)
                return;
            held_n = held.size();
            if (e == SpeechEvent::speech_start) {
                held.clear();
                holding = true;
            } else if (e == SpeechEvent::speech_end) {
                holding = false;
                // Open once local speech ends. Fresh model deltas continue into
                // the same queue; playback telemetry keeps the mic muted.
                play_held_locked("release");
                vad.reset_turn();
            }
        }
        if (vad_hook) vad_hook(e, level, held_n);
    }
};

TranslatePlaybackGate::TranslatePlaybackGate() : m_(std::make_unique<Impl>()) {}
TranslatePlaybackGate::~TranslatePlaybackGate() { close(); }

bool TranslatePlaybackGate::open(const Config& cfg, Sink sink, std::string& err) {
    close();
    m_->cfg = cfg;
    m_->sink = std::move(sink);
    m_->holding = (cfg.mode == PlaybackMode::conversational);
    m_->held.clear();
    m_->mute_logged = false;

    if (m_->cfg.mode == PlaybackMode::conversational) {
        if (!m_->vad.open(m_->cfg.vad, err)) return false;
        m_->vad.set_listener([this](SpeechEvent e, float level) { m_->on_vad(e, level); });
    }
    m_->opened = true;
    return true;
}

void TranslatePlaybackGate::close() {
    flush();
    m_->vad.close();
    m_->opened = false;
    m_->holding = false;
    m_->held.clear();
    m_->sink = nullptr;
    m_->vad_hook = nullptr;
    m_->playback_active = nullptr;
}

void TranslatePlaybackGate::set_vad_hook(VadHook hook) {
    if (m_) m_->vad_hook = std::move(hook);
}

void TranslatePlaybackGate::set_playback_active(PlaybackActive cb) {
    if (m_) m_->playback_active = std::move(cb);
}

void TranslatePlaybackGate::feed_mic(const std::int16_t* pcm, std::size_t frames, int rate_hz) {
    if (!m_ || !m_->opened || m_->cfg.mode != PlaybackMode::conversational) return;
    if (!pcm || frames == 0) return;

    // Half-duplex while queued or recently rendered translation is playing.
    // A turn that started before speaker playout must still reach speech_end.
    // New onsets remain blocked, preventing speaker echo from opening a turn.
    if (m_->playout_blocks_mic() && !m_->vad.in_speech()) return;
    if (m_->mute_logged) {
        m_->mute_logged = false;
        log::debug("translate gate: uplink=open");
    }

    const int want = m_->cfg.vad.sample_rate_hz;
    const std::int16_t* use = pcm;
    std::size_t use_n = frames;
    if (rate_hz != want) {
        downsample_to(pcm, frames, rate_hz, m_->mic_tmp, want);
        use = m_->mic_tmp.data();
        use_n = m_->mic_tmp.size();
    }
    m_->vad.feed(use, use_n);
}

void TranslatePlaybackGate::feed_translated(const std::int16_t* pcm, std::size_t frames) {
    if (!m_ || !m_->opened || !pcm || frames == 0) return;

    if (m_->cfg.mode == PlaybackMode::simultaneous) {
        if (m_->sink) m_->sink(pcm, frames);
        return;
    }

    std::lock_guard<std::mutex> lk(m_->mu);
    // Also hold while VAD already sees speech but onset has not fired yet.
    const bool hold = m_->holding.load() || m_->vad.in_speech();
    if (hold) {
        m_->holding = true;
        m_->append_held_locked(pcm, frames);
        return;
    }

    // Rest of this turn, including a short phrase that arrived after VAD end.
    if (m_->sink) m_->sink(pcm, frames);
}

void TranslatePlaybackGate::flush() {
    if (!m_) return;
    std::lock_guard<std::mutex> lk(m_->mu);
    if (!m_->held.empty()) {
        m_->play_held_locked("flush");
        m_->held.clear();
    }
    // Stay holding in conversational so a late flush cannot open pass-through
    // while the user may still be speaking at session teardown.
    m_->holding = (m_->cfg.mode == PlaybackMode::conversational);
}

PlaybackMode TranslatePlaybackGate::mode() const {
    return m_ ? m_->cfg.mode : PlaybackMode::simultaneous;
}
bool TranslatePlaybackGate::holding() const { return m_ && m_->holding.load(); }
bool TranslatePlaybackGate::uplink_muted() const {
    return m_ && m_->cfg.mode == PlaybackMode::conversational
        && m_->playout_blocks_mic();
}
std::size_t TranslatePlaybackGate::buffered_frames() const {
    if (!m_) return 0;
    std::lock_guard<std::mutex> lk(m_->mu);
    return m_->held.size();
}
const char* TranslatePlaybackGate::vad_backend() const {
    return m_ ? m_->vad.backend() : "none";
}

} // namespace pm::audio
