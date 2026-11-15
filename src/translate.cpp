// translate.cpp — RealtimeTranslateSession: mic/WAV + speaker + kbot TranslationSession.
// Host-compiled under FEATURE_REALTIME (needs kbot). See pm/audio/translate.hpp.

#include "pm/audio/translate.hpp"

#if defined(FEATURE_REALTIME) && FEATURE_REALTIME

#if defined(_MSC_VER)
#  pragma warning(push, 0)
#endif
#include "miniaudio.h"
#if defined(_MSC_VER)
#  pragma warning(pop)
#endif

#include "pm/audio/constants.hpp"
#include "pm/audio/devices.hpp"
#include "pm/audio/log.hpp"
#include "pm/audio/mic_activity.hpp"
#include "pm/audio/vad.hpp"
#include "process/cli_cancel.hpp"
#include "translation_session.h"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
#include <cstring>
#include <cctype>
#include <deque>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

namespace pm::audio {

namespace {

constexpr int kMicRate = static_cast<int>(constants::k_capture_sample_rate);
constexpr int kRtRate  = static_cast<int>(constants::k_realtime_sample_rate);
constexpr size_t kPlaybackPrebufFrames = static_cast<size_t>(kRtRate) / 5; // 200 ms
constexpr int kModelSilencePeak = 32;
constexpr double kModelSilenceRms = 4.0;

bool icontains(const std::string& haystack, const std::string& needle) {
    if (needle.empty()) return true;
    auto it = std::search(
        haystack.begin(), haystack.end(),
        needle.begin(), needle.end(),
        [](unsigned char a, unsigned char b) {
            return std::tolower(a) == std::tolower(b);
        });
    return it != haystack.end();
}

bool resolve_device_selector(const std::vector<DeviceInfo>& devices,
                             const std::string& selector,
                             const char* label,
                             std::string& resolved,
                             std::string& err)
{
    resolved.clear();
    if (selector.empty())
        return true;
    if (devices.empty()) {
        err = std::string("no ") + label + " devices found";
        return false;
    }
    std::vector<std::string> matches;
    for (const auto& d : devices) {
        if (icontains(d.name, selector))
            matches.push_back(d.name);
    }
    if (matches.empty()) {
        err = std::string("translate ") + label + " device not found: " + selector;
        return false;
    }
    if (matches.size() > 1) {
        err = std::string("translate ") + label + " device selector is ambiguous: " + selector;
        return false;
    }
    resolved = matches.front();
    return true;
}

class PcmFifo {
public:
    struct Snapshot {
        std::size_t queued_frames = 0;
        bool priming = false;
        std::int64_t prime_left_ms = 0;
        std::int64_t last_render_age_ms = -1;
        std::uint64_t pushed_frames = 0;
        std::uint64_t rendered_frames = 0;
        std::uint64_t underruns = 0;
        std::uint64_t refill_gaps = 0;
        std::uint64_t max_refill_gap_ms = 0;
        std::uint64_t reprime_skips = 0;
    };

    void set_start_delay_ms(int ms) {
        std::lock_guard<std::mutex> lk(mu_);
        start_delay_ms_ = std::max(0, ms);
    }
    void push(const int16_t* s, size_t n) {
        std::lock_guard<std::mutex> lk(mu_);
        const auto now = std::chrono::steady_clock::now();
        if (priming_ && prime_started_ == std::chrono::steady_clock::time_point{})
            prime_started_ = now;
        if (q_.empty() && last_rendered_ != std::chrono::steady_clock::time_point{}) {
            const auto gap = std::chrono::duration_cast<std::chrono::milliseconds>(
                now - last_rendered_).count();
            if (gap > 100) {
                ++refill_gaps_;
                max_refill_gap_ms_ = std::max<std::uint64_t>(
                    max_refill_gap_ms_, static_cast<std::uint64_t>(gap));
            }
        }
        constexpr size_t kMaxSamples = static_cast<size_t>(kRtRate) * 300;
        if (q_.size() + n > kMaxSamples) {
            const size_t drop = q_.size() + n - kMaxSamples;
            for (size_t i = 0; i < drop && !q_.empty(); ++i) q_.pop_front();
        }
        q_.insert(q_.end(), s, s + n);
        pushed_frames_ += n;
        if (q_.size() >= kPlaybackPrebufFrames)
            starved_ = false;
    }
    size_t pop(int16_t* out, size_t n) {
        std::lock_guard<std::mutex> lk(mu_);
        // Prime once at session start. Do NOT re-prime on underrun — OpenAI
        // streams in chunks. Conversational mode waits longer before each
        // distinct turn so late packets cannot split one spoken sentence.
        if (priming_) {
            if (q_.empty()) return 0;
            const auto elapsed = std::chrono::steady_clock::now() - prime_started_;
            if (elapsed < std::chrono::milliseconds(start_delay_ms_)) return 0;
            priming_ = false;
        }
        const size_t got = std::min(n, q_.size());
        for (size_t i = 0; i < got; ++i) { out[i] = q_.front(); q_.pop_front(); }
        if (got > 0)
            last_rendered_ = std::chrono::steady_clock::now();
        rendered_frames_ += got;
        if (got < n && got > 0 && !starved_) {
            ++underruns_;
            starved_ = true;
        }
        return got;
    }
    bool playback_active() {
        std::lock_guard<std::mutex> lk(mu_);
        const auto now = std::chrono::steady_clock::now();
        // Pre-buffer: queued PCM is not on the speaker yet — do not half-duplex
        // mute the mic, or the next utterance after a short pause is invisible
        // to VAD and uplink while playback_buffer_ms elapses.
        if (priming_ && !q_.empty()
            && prime_started_ != std::chrono::steady_clock::time_point{}) {
            const auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(
                now - prime_started_);
            if (elapsed < std::chrono::milliseconds(start_delay_ms_))
                return false;
        }
        if (!q_.empty()) return true;
        return last_rendered_ != std::chrono::steady_clock::time_point{}
            && now - last_rendered_ < std::chrono::milliseconds(500);
    }
    std::uint64_t underruns() {
        std::lock_guard<std::mutex> lk(mu_);
        return underruns_;
    }
    std::pair<std::uint64_t, std::uint64_t> refill_gap_stats() {
        std::lock_guard<std::mutex> lk(mu_);
        return {refill_gaps_, max_refill_gap_ms_};
    }
    void prime_next_push() {
        std::lock_guard<std::mutex> lk(mu_);
        const auto now = std::chrono::steady_clock::now();
        // The VAD callback runs after held PCM was pushed. Never re-prime an
        // in-progress queue: that creates a playback-buffer-sized pause in the
        // middle of one translation. Only arm a genuinely idle/new burst.
        if (!priming_
            && last_rendered_ != std::chrono::steady_clock::time_point{}
            && now - last_rendered_ < std::chrono::milliseconds(500)) {
            ++reprime_skips_;
            return;
        }
        priming_ = true;
        prime_started_ = now;
    }
    std::uint64_t reprime_skips() {
        std::lock_guard<std::mutex> lk(mu_);
        return reprime_skips_;
    }
    Snapshot snapshot() {
        std::lock_guard<std::mutex> lk(mu_);
        Snapshot out;
        const auto now = std::chrono::steady_clock::now();
        out.queued_frames = q_.size();
        out.priming = priming_;
        if (priming_ && prime_started_ != std::chrono::steady_clock::time_point{}) {
            const auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(
                now - prime_started_).count();
            out.prime_left_ms = std::max<std::int64_t>(0, start_delay_ms_ - elapsed);
        }
        if (last_rendered_ != std::chrono::steady_clock::time_point{}) {
            out.last_render_age_ms = std::chrono::duration_cast<std::chrono::milliseconds>(
                now - last_rendered_).count();
        }
        out.pushed_frames = pushed_frames_;
        out.rendered_frames = rendered_frames_;
        out.underruns = underruns_;
        out.refill_gaps = refill_gaps_;
        out.max_refill_gap_ms = max_refill_gap_ms_;
        out.reprime_skips = reprime_skips_;
        return out;
    }
    void reset_prime() {
        std::lock_guard<std::mutex> lk(mu_);
        q_.clear();
        priming_ = true;
        prime_started_ = {};
        last_rendered_ = {};
        starved_ = false;
        underruns_ = 0;
        refill_gaps_ = 0;
        max_refill_gap_ms_ = 0;
        reprime_skips_ = 0;
        pushed_frames_ = 0;
        rendered_frames_ = 0;
    }
private:
    std::mutex mu_;
    std::deque<int16_t> q_;
    bool priming_ = true;
    int start_delay_ms_ = 200;
    std::chrono::steady_clock::time_point prime_started_{};
    bool starved_ = false;
    std::uint64_t underruns_ = 0;
    std::uint64_t refill_gaps_ = 0;
    std::uint64_t max_refill_gap_ms_ = 0;
    std::uint64_t reprime_skips_ = 0;
    std::uint64_t pushed_frames_ = 0;
    std::uint64_t rendered_frames_ = 0;
    std::chrono::steady_clock::time_point last_rendered_{};
};

bool decode_wav_mono16(const std::string& path, int want_rate,
                       std::vector<int16_t>& pcm, std::string& err) {
    pcm.clear();
    ma_decoder_config cfg = ma_decoder_config_init(ma_format_s16, 1, static_cast<ma_uint32>(want_rate));
    ma_decoder dec{};
    if (ma_decoder_init_file(path.c_str(), &cfg, &dec) != MA_SUCCESS) {
        err = "translate --src: cannot open WAV: " + path;
        return false;
    }
    ma_uint64 frames = 0;
    if (ma_decoder_get_length_in_pcm_frames(&dec, &frames) != MA_SUCCESS || frames == 0) {
        // Length unknown — read in chunks.
        std::vector<int16_t> chunk(static_cast<size_t>(want_rate));
        for (;;) {
            ma_uint64 got = 0;
            if (ma_decoder_read_pcm_frames(&dec, chunk.data(), chunk.size(), &got) != MA_SUCCESS)
                break;
            if (got == 0) break;
            pcm.insert(pcm.end(), chunk.begin(), chunk.begin() + static_cast<std::ptrdiff_t>(got));
        }
    } else {
        pcm.resize(static_cast<size_t>(frames));
        ma_uint64 got = 0;
        if (ma_decoder_read_pcm_frames(&dec, pcm.data(), frames, &got) != MA_SUCCESS || got == 0) {
            ma_decoder_uninit(&dec);
            err = "translate --src: empty or unreadable WAV: " + path;
            return false;
        }
        pcm.resize(static_cast<size_t>(got));
    }
    ma_decoder_uninit(&dec);
    if (pcm.empty()) {
        err = "translate --src: no PCM in " + path;
        return false;
    }
    return true;
}

} // namespace

struct RealtimeTranslateSession::Impl {
    RealtimeTranslateSession* owner = nullptr;
    Config    cfg;
    Callbacks cb;

    polymech::kbot::TranslationSession session;
    AudioInput mic;
    PcmFifo    fifo;
    TranslatePlaybackGate gate;

    ma_device    play_dev{};
    bool         play_open = false;
    ma_resampler resampler{};
    bool         resampler_open = false;
    std::vector<int16_t> resamp_out;
    std::vector<int16_t> muted_capture;
    bool         uplink_silence_logged = false;
    bool         model_silence_logged = false;
    std::atomic<std::uint64_t> model_silent_frames{0};
    std::atomic<std::uint64_t> model_audio_frames{0};
    ma_resampler render_resampler{};
    bool         render_resampler_open = false;
    std::vector<int16_t> render_ref_out;

    std::atomic<bool> active{false};
    std::atomic<bool> stopping{false};
    std::atomic<bool> failed{false};
    std::atomic<bool> file_done{false};
    std::chrono::steady_clock::time_point last_level{};
    std::chrono::steady_clock::time_point last_diag{};
    std::unique_ptr<MicActivityScope> mic_scope;
    std::thread file_thread;
    bool mic_started = false;

    static void play_cb(ma_device* dev, void* out, const void* /*in*/, ma_uint32 frames) {
        auto* self = static_cast<Impl*>(dev->pUserData);
        auto* o = static_cast<int16_t*>(out);
        const size_t got = self ? self->fifo.pop(o, frames) : 0;
        if (got < frames)
            std::memset(o + got, 0, (frames - got) * sizeof(int16_t));
        if (self && self->active.load() && self->render_resampler_open && frames > 0) {
            ma_uint64 in_frames  = frames;
            ma_uint64 out_frames = (static_cast<size_t>(frames) * static_cast<size_t>(kMicRate)
                                    + static_cast<size_t>(kRtRate) - 1)
                                   / static_cast<size_t>(kRtRate) + 8;
            self->render_ref_out.resize(static_cast<size_t>(out_frames));
            if (ma_resampler_process_pcm_frames(
                    &self->render_resampler, o, &in_frames,
                    self->render_ref_out.data(), &out_frames) == MA_SUCCESS
                && out_frames > 0) {
                self->mic.process_render_reference(
                    self->render_ref_out.data(), static_cast<size_t>(out_frames));
            }
        }
    }

    void fire_level(const int16_t* pcm, size_t frames) {
        if (!cb.on_level || frames == 0) return;
        const auto now = std::chrono::steady_clock::now();
        if (now - last_level < std::chrono::milliseconds(80)) return;
        last_level = now;
        double sum = 0.0;
        for (size_t i = 0; i < frames; ++i) {
            const double s = static_cast<double>(pcm[i]) / 32768.0;
            sum += s * s;
        }
        const double level = std::min(1.0, std::sqrt(sum / static_cast<double>(frames)) * 4.0);
        cb.on_level(level);
        mic_notify_level(level);
    }

    void log_playout_diag(bool muted) {
        if (!cb.on_vad || cfg.playback_mode != PlaybackMode::conversational
            || !log::debug_enabled())
            return;
        const auto now = std::chrono::steady_clock::now();
        if (now - last_diag < std::chrono::seconds(5))
            return;
        last_diag = now;
        const auto s = fifo.snapshot();
        const auto queued_ms = static_cast<std::uint64_t>(
            s.queued_frames * 1000ull / static_cast<std::uint64_t>(kRtRate));
        log::debug("translate diag: queued_ms=" + std::to_string(queued_ms)
                   + " priming=" + std::to_string(s.priming ? 1 : 0)
                   + " prime_left_ms=" + std::to_string(s.prime_left_ms)
                   + " last_render_age_ms=" + std::to_string(s.last_render_age_ms)
                   + " pushed_frames=" + std::to_string(s.pushed_frames)
                   + " rendered_frames=" + std::to_string(s.rendered_frames)
                   + " model_audio_frames=" + std::to_string(model_audio_frames.load())
                   + " model_silent_frames=" + std::to_string(model_silent_frames.load())
                   + " underruns=" + std::to_string(s.underruns)
                   + " refill_gaps=" + std::to_string(s.refill_gaps)
                   + " max_refill_gap_ms=" + std::to_string(s.max_refill_gap_ms)
                   + " reprime_skips=" + std::to_string(s.reprime_skips)
                   + " gate_holding=" + std::to_string(gate.holding() ? 1 : 0)
                   + " gate_buffered_frames=" + std::to_string(gate.buffered_frames())
                   + " uplink=" + (muted ? "silence" : "open"));
    }

    void on_model_pcm(const int16_t* pcm, size_t frames) {
        if (!pcm || frames == 0)
            return;
        std::uint64_t sum_sq = 0;
        int peak = 0;
        for (size_t i = 0; i < frames; ++i) {
            const int sample = static_cast<int>(pcm[i]);
            const int magnitude = sample < 0 ? -sample : sample;
            peak = std::max(peak, magnitude);
            sum_sq += static_cast<std::uint64_t>(
                static_cast<std::int64_t>(sample) * static_cast<std::int64_t>(sample));
        }
        const double rms = std::sqrt(
            static_cast<double>(sum_sq) / static_cast<double>(frames));
        // Realtime translation emits continuous digital silence while we feed
        // silence during half-duplex playout. Do not let that silence keep the
        // speaker FIFO—and therefore the mic mute—alive forever.
        if (peak <= kModelSilencePeak && rms <= kModelSilenceRms) {
            model_silent_frames.fetch_add(frames);
            if (!model_silence_logged) {
                model_silence_logged = true;
                log::debug("translate model output=silence (not queued)");
            }
            return;
        }
        model_audio_frames.fetch_add(frames);
        if (model_silence_logged) {
            model_silence_logged = false;
            log::debug("translate model output=audio peak=" + std::to_string(peak)
                       + " rms=" + std::to_string(static_cast<int>(rms)));
        }
        gate.feed_translated(pcm, frames);
    }

    void on_mic_pcm(const int16_t* pcm, size_t frames) {
        if (!active.load() || !pcm || frames == 0) return;
        fire_level(pcm, frames);

        std::vector<int16_t> gained;
        const int16_t* src = pcm;
        size_t n = frames;
        if (cfg.mic_gain != 1.0f) {
            gained.resize(frames);
            for (size_t i = 0; i < frames; ++i) {
                const float v = static_cast<float>(pcm[i]) * cfg.mic_gain;
                gained[i] = static_cast<int16_t>(
                    std::max(-32768.f, std::min(32767.f, v)));
            }
            src = gained.data();
        }

        // VAD on capture-rate mic (AEC-cleaned when APM is active).
        gate.feed_mic(src, n, kMicRate);
        // Translation sessions require continuous real-time input. During
        // playout send silence—not speaker echo, and not no packets—so model
        // time and its output stream continue without stalling.
        const bool muted = gate.uplink_muted();
        if (muted) {
            muted_capture.assign(n, 0);
            src = muted_capture.data();
            if (!uplink_silence_logged) {
                uplink_silence_logged = true;
                log::debug("translate gate: uplink=silence");
            }
        } else {
            uplink_silence_logged = false;
        }
        log_playout_diag(muted);

        if (!resampler_open) return;
        ma_uint64 in_frames  = n;
        ma_uint64 out_frames = (n * static_cast<size_t>(kRtRate) + kMicRate - 1)
                               / static_cast<size_t>(kMicRate) + 8;
        resamp_out.resize(static_cast<size_t>(out_frames));
        const ma_result r = ma_resampler_process_pcm_frames(
            &resampler, src, &in_frames, resamp_out.data(), &out_frames);
        if (r != MA_SUCCESS) {
            log::warn("RealtimeTranslateSession: resampler error "
                      + std::to_string(static_cast<int>(r)));
            return;
        }
        if (out_frames > 0)
            session.send_pcm(resamp_out.data(), static_cast<size_t>(out_frames));
    }

    void run_file_feeder(std::vector<int16_t> pcm) {
        constexpr size_t kChunk = static_cast<size_t>(kMicRate) / 50; // 20 ms
        auto next = std::chrono::steady_clock::now();
        do {
            size_t off = 0;
            while (active.load() && !stopping.load() && off < pcm.size()) {
                const size_t n = std::min(kChunk, pcm.size() - off);
                on_mic_pcm(pcm.data() + off, n);
                off += n;
                next += std::chrono::milliseconds(20);
                std::this_thread::sleep_until(next);
            }
        } while (cfg.input_loop && active.load() && !stopping.load());

        // Trailing silence so conversational VAD can reach speech_end after EOF.
        if (active.load() && !stopping.load()
            && cfg.playback_mode == PlaybackMode::conversational) {
            const int pad_ms = std::max(cfg.silence_ms + 300, 900);
            const int pads = (pad_ms + 19) / 20;
            std::vector<int16_t> quiet(kChunk, 0);
            for (int i = 0; i < pads && active.load() && !stopping.load(); ++i) {
                on_mic_pcm(quiet.data(), quiet.size());
                next += std::chrono::milliseconds(20);
                std::this_thread::sleep_until(next);
            }
        }

        file_done.store(true);
        log::debug("RealtimeTranslateSession: --src feed finished"
                   + std::string(cfg.input_loop ? " (loop ended)" : ""));
    }

    void cleanup_local_audio() {
        stopping.store(true);
        if (file_thread.joinable())
            file_thread.join();
        try { mic.stop(); } catch (...) {}
        mic_started = false;
        mic_scope.reset();
        gate.flush();
        gate.close();
        const auto playout_underruns = fifo.underruns();
        const auto [refill_gaps, max_refill_gap_ms] = fifo.refill_gap_stats();
        const auto reprime_skips = fifo.reprime_skips();
        log::debug("RealtimeTranslateSession: playout underruns="
                   + std::to_string(playout_underruns)
                   + " refill_gaps=" + std::to_string(refill_gaps)
                   + " max_refill_gap_ms=" + std::to_string(max_refill_gap_ms)
                   + " reprime_skips=" + std::to_string(reprime_skips));
        if (play_open) {
            ma_device_stop(&play_dev);
            ma_device_uninit(&play_dev);
            play_open = false;
        }
        fifo.reset_prime();
        if (resampler_open) {
            ma_resampler_uninit(&resampler, nullptr);
            resampler_open = false;
        }
        if (render_resampler_open) {
            ma_resampler_uninit(&render_resampler, nullptr);
            render_resampler_open = false;
        }
    }
};

namespace {

std::mutex& reg_mu() { static std::mutex m; return m; }
std::vector<RealtimeTranslateSession*>& reg() {
    static std::vector<RealtimeTranslateSession*> v;
    return v;
}
std::once_flag g_cancel_once;

void ensure_cancel_listener() {
    std::call_once(g_cancel_once, [] {
        media::cli::register_cancel_listener([] {
            std::vector<RealtimeTranslateSession*> snapshot;
            {
                std::lock_guard<std::mutex> lk(reg_mu());
                snapshot = reg();
            }
            // Ctrl+Break ends the process when this handler returns, so the
            // session has to stop here. stop() is idempotent with the main loop.
            log::info("RealtimeTranslateSession: cancel → stopping "
                      + std::to_string(snapshot.size()) + " session(s)");
            for (auto* s : snapshot)
                if (s) s->stop();
        });
    });
}

void reg_add(RealtimeTranslateSession* s) {
    std::lock_guard<std::mutex> lk(reg_mu());
    reg().push_back(s);
}
void reg_remove(RealtimeTranslateSession* s) {
    std::lock_guard<std::mutex> lk(reg_mu());
    auto& v = reg();
    v.erase(std::remove(v.begin(), v.end(), s), v.end());
}

} // namespace

RealtimeTranslateSession::RealtimeTranslateSession()
    : m_(std::make_unique<Impl>()) { m_->owner = this; }
RealtimeTranslateSession::~RealtimeTranslateSession() { stop(); }

bool RealtimeTranslateSession::is_active() const { return m_->active.load(); }

bool RealtimeTranslateSession::input_exhausted() const {
    return !m_->cfg.input_wav.empty() && !m_->cfg.input_loop && m_->file_done.load();
}

bool RealtimeTranslateSession::start(const Config& cfg, Callbacks cb, std::string& err) {
    if (m_->active.load()) { err = "translate session already active"; return false; }
    if (cfg.language.empty()) { err = "translate language is required"; return false; }
    m_->cfg = cfg;
    m_->cb  = std::move(cb);
    m_->stopping.store(false);
    m_->failed.store(false);
    m_->file_done.store(false);
    m_->model_silence_logged = false;
    m_->model_silent_frames.store(0);
    m_->model_audio_frames.store(0);
    m_->last_diag = {};

    if (m_->cfg.silence_ms < 200) m_->cfg.silence_ms = 200;
    if (m_->cfg.silence_ms > 2000) m_->cfg.silence_ms = 2000;
    m_->cfg.playback_buffer_ms = std::clamp(m_->cfg.playback_buffer_ms, 0, 5000);
    m_->fifo.set_start_delay_ms(
        m_->cfg.playback_mode == PlaybackMode::conversational
            ? m_->cfg.playback_buffer_ms : 200);

    const bool use_file = !cfg.input_wav.empty();
    std::vector<int16_t> file_pcm;
    if (use_file) {
        if (!decode_wav_mono16(cfg.input_wav, kMicRate, file_pcm, err))
            return false;
        log::debug("RealtimeTranslateSession: --src \"" + cfg.input_wav + "\" frames="
                   + std::to_string(file_pcm.size()) + " @" + std::to_string(kMicRate)
                   + "Hz loop=" + (cfg.input_loop ? "1" : "0"));
    }

    std::string resolved_mic;
    if (!use_file) {
        if (!resolve_device_selector(enumerate_capture_devices(), cfg.mic_device,
                                     "capture", resolved_mic, err))
            return false;
    }
    std::string resolved_speaker;
    if (!resolve_device_selector(enumerate_playback_devices(), cfg.speaker_device,
                                 "playback", resolved_speaker, err))
        return false;

    ensure_cancel_listener();
    reg_add(this);

    {
        ma_device_config dc = ma_device_config_init(ma_device_type_playback);
        dc.playback.format   = ma_format_s16;
        dc.playback.channels = 1;
        dc.sampleRate        = kRtRate;
        dc.dataCallback      = Impl::play_cb;
        dc.pUserData         = m_.get();
        ma_device_id playback_id{};
        bool found_playback_id = false;
        if (!resolved_speaker.empty()) {
            ma_context ctx{};
            if (ma_context_init(nullptr, 0, nullptr, &ctx) == MA_SUCCESS) {
                ma_device_info* pCapture = nullptr;
                ma_uint32 nCapture = 0;
                ma_device_info* pPlayback = nullptr;
                ma_uint32 nPlayback = 0;
                if (ma_context_get_devices(&ctx, &pPlayback, &nPlayback, &pCapture, &nCapture)
                    == MA_SUCCESS) {
                    for (ma_uint32 i = 0; i < nPlayback; ++i) {
                        if (pPlayback[i].name == resolved_speaker) {
                            playback_id = pPlayback[i].id;
                            found_playback_id = true;
                            break;
                        }
                    }
                }
                ma_context_uninit(&ctx);
            }
            if (!found_playback_id) {
                err = "translate playback device disappeared before open: " + resolved_speaker;
                reg_remove(this);
                return false;
            }
            dc.playback.pDeviceID = &playback_id;
        }
        if (ma_device_init(nullptr, &dc, &m_->play_dev) != MA_SUCCESS) {
            err = "ma_device_init (playback) failed";
            reg_remove(this);
            return false;
        }
        m_->play_open = true;
        if (ma_device_start(&m_->play_dev) != MA_SUCCESS) {
            err = "ma_device_start (playback) failed";
            ma_device_uninit(&m_->play_dev); m_->play_open = false;
            reg_remove(this);
            return false;
        }
    }

    {
        ma_resampler_config rc = ma_resampler_config_init(
            ma_format_s16, 1, kMicRate, kRtRate, ma_resample_algorithm_linear);
        if (ma_resampler_init(&rc, nullptr, &m_->resampler) != MA_SUCCESS) {
            err = "ma_resampler_init failed";
            ma_device_uninit(&m_->play_dev); m_->play_open = false;
            reg_remove(this);
            return false;
        }
        m_->resampler_open = true;
    }
    {
        ma_resampler_config rc = ma_resampler_config_init(
            ma_format_s16, 1, kRtRate, kMicRate, ma_resample_algorithm_linear);
        if (ma_resampler_init(&rc, nullptr, &m_->render_resampler) != MA_SUCCESS) {
            err = "ma_resampler_init (render reference) failed";
            ma_resampler_uninit(&m_->resampler, nullptr); m_->resampler_open = false;
            ma_device_uninit(&m_->play_dev); m_->play_open = false;
            reg_remove(this);
            return false;
        }
        m_->render_resampler_open = true;
    }

    polymech::kbot::TranslationSession::Config sc;
    sc.base_url                 = cfg.base_url;
    sc.api_key                  = cfg.api_key;
    sc.model                    = cfg.model;
    sc.language                 = cfg.language;
    sc.noise_reduction          = cfg.noise_reduction;
    sc.transcription_model      = cfg.transcription_model;
    sc.input_rate               = kRtRate;
    sc.output_rate              = kRtRate;
    sc.close_drain_timeout_ms   = cfg.close_drain_timeout_ms;

    auto session_ready = std::make_shared<std::atomic<bool>>(false);
    Impl* impl = m_.get();
    {
        TranslatePlaybackGate::Config gc;
        gc.mode = m_->cfg.playback_mode;
        gc.vad.silence_ms = m_->cfg.silence_ms;
        gc.vad.sample_rate_hz = kMicRate;
        gc.translated_rate_hz = kRtRate;
        std::string gerr;
        if (!m_->gate.open(gc, [impl](const int16_t* pcm, size_t n) {
                impl->fifo.push(pcm, n);
            }, gerr)) {
            err = gerr.empty() ? "translate playback gate failed" : gerr;
            m_->cleanup_local_audio();
            reg_remove(this);
            return false;
        }
        m_->gate.set_playback_active([impl] { return impl->fifo.playback_active(); });
        if (m_->cb.on_vad) {
            m_->gate.set_vad_hook([impl](SpeechEvent e, float level, std::size_t held) {
                if (e == SpeechEvent::speech_end)
                    impl->fifo.prime_next_push();
                if (impl->cb.on_vad) impl->cb.on_vad(e, level, held);
            });
        } else {
            m_->gate.set_vad_hook([impl](SpeechEvent e, float, std::size_t) {
                if (e == SpeechEvent::speech_end)
                    impl->fifo.prime_next_push();
            });
        }
        log::info(std::string("RealtimeTranslateSession: playback=")
                  + playback_mode_name(m_->cfg.playback_mode)
                  + " silence_ms=" + std::to_string(m_->cfg.silence_ms)
                  + " vad=" + m_->gate.vad_backend());
    }
    polymech::kbot::TranslationSession::Callbacks scb;
    scb.on_audio_pcm = [impl](const int16_t* pcm, size_t n) {
        impl->on_model_pcm(pcm, n);
    };
    scb.on_output_transcript = [impl](const std::string& d) {
        if (impl->cb.on_output_transcript) impl->cb.on_output_transcript(d);
    };
    scb.on_input_transcript = [impl](const std::string& d) {
        if (impl->cb.on_input_transcript) impl->cb.on_input_transcript(d);
    };
    scb.on_error = [impl](const std::string& m) {
        impl->failed.store(true);
        if (impl->cb.on_error) impl->cb.on_error(m);
    };
    scb.on_auth_error = [impl](const std::string& m) {
        if (impl->cb.on_auth_error) impl->cb.on_auth_error(m);
    };
    scb.on_network_error = [impl](const std::string& m) {
        if (impl->cb.on_network_error) impl->cb.on_network_error(m);
    };
    scb.on_state = [impl, session_ready](bool connected) {
        if (connected) {
            session_ready->store(true);
            return;
        }
        if (impl->stopping.load()) return;
        log::warn("RealtimeTranslateSession: session closed unexpectedly");
        if (impl->stopping.exchange(true)) return;
        impl->active.store(false);
        impl->cleanup_local_audio();
        if (impl->owner) reg_remove(impl->owner);
        if (impl->cb.on_closed) impl->cb.on_closed();
        impl->stopping.store(false);
    };

    if (!m_->session.open(sc, std::move(scb), err)) {
        m_->cleanup_local_audio();
        reg_remove(this);
        return false;
    }

    m_->mic_scope = std::make_unique<MicActivityScope>();
    m_->active.store(true);

    if (use_file) {
        m_->file_thread = std::thread([impl, pcm = std::move(file_pcm)]() mutable {
            impl->run_file_feeder(std::move(pcm));
        });
    } else {
        try {
            const std::string& mic_sel =
                resolved_mic.empty() ? cfg.mic_device : resolved_mic;
            m_->mic.start([impl](const int16_t* pcm, size_t n) { impl->on_mic_pcm(pcm, n); },
                          mic_sel);
            m_->mic_started = true;
        } catch (const std::exception& e) {
            err = std::string("mic start failed: ") + e.what();
            m_->active.store(false);
            m_->mic_scope.reset();
            m_->session.close();
            m_->cleanup_local_audio();
            reg_remove(this);
            return false;
        }

        constexpr int kAecStreamDelayMs =
            static_cast<int>(kPlaybackPrebufFrames * 1000 / static_cast<size_t>(kRtRate));
        m_->mic.set_aec_stream_delay_ms(kAecStreamDelayMs);
    }

    if (m_->failed.load() || !m_->session.is_open()) {
        err = m_->failed.load() ? std::string()
                                : std::string("translate session closed during startup");
        m_->active.store(false);
        try { m_->mic.stop(); } catch (...) {}
        m_->mic_scope.reset();
        m_->session.close();
        m_->cleanup_local_audio();
        reg_remove(this);
        return false;
    }

    const auto ready_deadline = std::chrono::steady_clock::now() + std::chrono::seconds(10);
    while (!session_ready->load()
           && !m_->failed.load()
           && m_->session.is_open()
           && std::chrono::steady_clock::now() < ready_deadline) {
        std::this_thread::sleep_for(std::chrono::milliseconds(25));
    }
    if (!session_ready->load()) {
        err = m_->failed.load() ? std::string()
                                : std::string("translate session did not become ready");
        m_->active.store(false);
        try { m_->mic.stop(); } catch (...) {}
        m_->mic_scope.reset();
        m_->session.close();
        m_->cleanup_local_audio();
        reg_remove(this);
        return false;
    }

    log::info("RealtimeTranslateSession: live lang=" + cfg.language
              + " model=" + cfg.model
              + (use_file ? " src=wav" : (" mic=\"" + m_->mic.opened_device_name() + "\"")));
    if (m_->cb.on_ready) m_->cb.on_ready();
    return true;
}

void RealtimeTranslateSession::stop() {
    if (m_->stopping.exchange(true)) return;
    if (!m_->active.exchange(false) && !m_->play_open && !m_->resampler_open
        && !m_->render_resampler_open) {
        m_->stopping.store(false);
        return;
    }

    try { m_->mic.stop(); } catch (...) {}
    m_->mic_scope.reset();

    std::string ferr;
    if (!m_->session.finish(ferr)) {
        if (!ferr.empty())
            log::warn("RealtimeTranslateSession: finish: " + ferr);
        m_->session.close();
    }

    if (m_->play_open)
        std::this_thread::sleep_for(std::chrono::milliseconds(200));
    m_->cleanup_local_audio();

    reg_remove(this);
    log::info("RealtimeTranslateSession: stopped");
    if (m_->cb.on_closed) m_->cb.on_closed();
    m_->stopping.store(false);
}

void RealtimeTranslateSession::update_language(const std::string& language) {
    if (m_->active.load()) {
        m_->cfg.language = language;
        m_->session.update_language(language);
    }
}

} // namespace pm::audio

#endif // FEATURE_REALTIME
