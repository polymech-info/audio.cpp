// translate.cpp — RealtimeTranslateSession: mic + speaker + kbot TranslationSession.
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
    void push(const int16_t* s, size_t n) {
        std::lock_guard<std::mutex> lk(mu_);
        constexpr size_t kMaxSamples = static_cast<size_t>(kRtRate) * 300;
        if (q_.size() + n > kMaxSamples) {
            const size_t drop = q_.size() + n - kMaxSamples;
            for (size_t i = 0; i < drop && !q_.empty(); ++i) q_.pop_front();
        }
        q_.insert(q_.end(), s, s + n);
    }
    size_t pop(int16_t* out, size_t n) {
        std::lock_guard<std::mutex> lk(mu_);
        if (priming_) {
            if (q_.size() < kPlaybackPrebufFrames) return 0;
            priming_ = false;
        }
        const size_t got = std::min(n, q_.size());
        for (size_t i = 0; i < got; ++i) { out[i] = q_.front(); q_.pop_front(); }
        if (q_.empty()) priming_ = true;
        return got;
    }
    void clear() { std::lock_guard<std::mutex> lk(mu_); q_.clear(); priming_ = true; }
private:
    std::mutex mu_;
    std::deque<int16_t> q_;
    bool priming_ = true;
};

} // namespace

struct RealtimeTranslateSession::Impl {
    RealtimeTranslateSession* owner = nullptr;
    Config    cfg;
    Callbacks cb;

    polymech::kbot::TranslationSession session;
    AudioInput mic;
    PcmFifo    fifo;

    ma_device    play_dev{};
    bool         play_open = false;
    ma_resampler resampler{};
    bool         resampler_open = false;
    std::vector<int16_t> resamp_out;
    ma_resampler render_resampler{};
    bool         render_resampler_open = false;
    std::vector<int16_t> render_ref_out;

    std::atomic<bool> active{false};
    std::atomic<bool> stopping{false};
    std::atomic<bool> failed{false};
    std::chrono::steady_clock::time_point last_level{};
    std::unique_ptr<MicActivityScope> mic_scope;

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

    void cleanup_local_audio() {
        try { mic.stop(); } catch (...) {}
        mic_scope.reset();
        if (play_open) {
            ma_device_stop(&play_dev);
            ma_device_uninit(&play_dev);
            play_open = false;
        }
        fifo.clear();
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

bool RealtimeTranslateSession::start(const Config& cfg, Callbacks cb, std::string& err) {
    if (m_->active.load()) { err = "translate session already active"; return false; }
    if (cfg.language.empty()) { err = "translate language is required"; return false; }
    m_->cfg = cfg;
    m_->cb  = std::move(cb);
    m_->stopping.store(false);
    m_->failed.store(false);

    std::string resolved_mic;
    if (!resolve_device_selector(enumerate_capture_devices(), cfg.mic_device,
                                 "capture", resolved_mic, err))
        return false;
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
    polymech::kbot::TranslationSession::Callbacks scb;
    scb.on_audio_pcm = [impl](const int16_t* pcm, size_t n) { impl->fifo.push(pcm, n); };
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

    try {
        const std::string& mic_sel =
            resolved_mic.empty() ? cfg.mic_device : resolved_mic;
        m_->mic.start([impl](const int16_t* pcm, size_t n) { impl->on_mic_pcm(pcm, n); },
                      mic_sel);
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
              + " mic=\"" + m_->mic.opened_device_name() + "\"");
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
