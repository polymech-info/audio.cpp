
#ifndef PM_HAS_AUDIO_IO
#define PM_HAS_AUDIO_IO 1
#endif

#if PM_HAS_AUDIO_IO

#include "pm/audio/mixed_capture.hpp"

#include "pm/audio/devices.hpp"
#include "pm/audio/constants.hpp"

#include <algorithm>
#include <atomic>
#include <cmath>
#include <cctype>
#include <deque>
#include <mutex>
#include <unordered_set>
#include <vector>
#include "string/string_utils.hpp"

namespace pm::audio {
namespace {

std::mutex& capture_cancel_mu()
{
    static std::mutex m;
    return m;
}
std::vector<MixedAudioCapture*>& capture_cancel_reg()
{
    static std::vector<MixedAudioCapture*> v;
    return v;
}
std::once_flag g_capture_cancel_once;
CancelSubscribeFn g_cancel_subscribe = nullptr;
CameraNameFn g_camera_name = nullptr;

void stop_registered_captures()
{
    std::vector<MixedAudioCapture*> snap;
    {
        std::lock_guard<std::mutex> lk(capture_cancel_mu());
        snap = capture_cancel_reg();
    }
    for (auto* c : snap) {
        if (c)
            c->stop();
    }
}

void ensure_capture_cancel_listener()
{
    std::call_once(g_capture_cancel_once, [] {
        if (g_cancel_subscribe)
            g_cancel_subscribe(&stop_registered_captures);
    });
}

void capture_cancel_add(MixedAudioCapture* c)
{
    if (!c)
        return;
    ensure_capture_cancel_listener();
    std::lock_guard<std::mutex> lk(capture_cancel_mu());
    auto& v = capture_cancel_reg();
    if (std::find(v.begin(), v.end(), c) == v.end())
        v.push_back(c);
}

void capture_cancel_remove(MixedAudioCapture* c)
{
    std::lock_guard<std::mutex> lk(capture_cancel_mu());
    auto& v = capture_cancel_reg();
    v.erase(std::remove(v.begin(), v.end(), c), v.end());
}

using media::strutil::lower_ascii;
bool icontains(const std::string& hay, const std::string& needle)
{
    if (needle.empty()) return true;
    const auto h = lower_ascii(hay);
    const auto n = lower_ascii(needle);
    return h.find(n) != std::string::npos;
}

void apply_gain_interleaved(const int16_t* in,
                            size_t sample_count,
                            float gain,
                            std::vector<int16_t>& out)
{
    out.resize(sample_count);
    if (gain == 1.0f) {
        std::copy(in, in + sample_count, out.begin());
        return;
    }
    for (size_t i = 0; i < sample_count; ++i) {
        const float s = static_cast<float>(in[i]) * gain;
        out[i] = static_cast<int16_t>(std::clamp(s, -32768.0f, 32767.0f));
    }
}

std::vector<std::string> tokenize_name(const std::string& name)
{
    static const std::unordered_set<std::string> k_stop = {
        "microphone", "mic", "webcam", "camera", "usb", "hd", "pro",
        "the", "and", "for", "audio", "input", "device", "array",
        "digital", "stereo", "mono", "default",
    };
    std::vector<std::string> out;
    std::string cur;
    auto flush = [&] {
        if (cur.size() < 3) { cur.clear(); return; }
        const auto t = lower_ascii(cur);
        cur.clear();
        if (k_stop.count(t)) return;
        out.push_back(t);
    };
    for (unsigned char uc : name) {
        const char c = static_cast<char>(uc);
        if (std::isalnum(uc)) {
            cur.push_back(c);
        } else {
            flush();
        }
    }
    flush();
    return out;
}

int score_audio_for_camera(const std::string& audio_name, const std::string& camera_name)
{
    if (camera_name.empty()) return 0;
    if (icontains(audio_name, camera_name) || icontains(camera_name, audio_name))
        return 1000 + static_cast<int>(std::min(audio_name.size(), camera_name.size()));

    const auto cam_tok = tokenize_name(camera_name);
    const auto aud_tok = tokenize_name(audio_name);
    if (cam_tok.empty() || aud_tok.empty()) return 0;
    int score = 0;
    for (const auto& c : cam_tok) {
        for (const auto& a : aud_tok) {
            if (c == a) score += static_cast<int>(c.size()) * 10;
            else if (c.size() >= 4 && (a.find(c) != std::string::npos ||
                                       c.find(a) != std::string::npos))
                score += static_cast<int>(std::min(c.size(), a.size())) * 4;
        }
    }
    return score;
}

void push_side_buf(std::deque<int16_t>& buf,
                   size_t cap,
                   const int16_t* pcm,
                   size_t samples)
{
    buf.insert(buf.end(), pcm, pcm + samples);
    if (buf.size() > cap)
        buf.erase(buf.begin(),
                  buf.begin() + static_cast<std::ptrdiff_t>(buf.size() - cap));
}

float take_side_sample(std::deque<int16_t>& buf, size_t i, size_t avail, float gain)
{
    if (i >= avail) return 0.0f;
    return static_cast<float>(buf[i]) * gain;
}

} // namespace

void set_camera_name_for_capture(CameraNameFn fn)
{
    g_camera_name = fn;
}

void set_mixed_capture_cancel_subscribe(CancelSubscribeFn fn)
{
    g_cancel_subscribe = fn;
}

std::string resolve_camera_audio_device(const std::string& camera_input)
{
    std::string video_name = camera_input;
    if (g_camera_name) {
        const std::string named = g_camera_name(camera_input);
        if (!named.empty())
            video_name = named;
    }

    const auto audios = enumerate_capture_devices();
    if (audios.empty())
        return camera_input;

    int best_score = 0;
    std::string best;
    const std::string needle = !video_name.empty() ? video_name : camera_input;
    for (const auto& a : audios) {
        if (a.is_loopback) continue;
        int score = 0;
        if (!camera_input.empty() && icontains(a.name, camera_input))
            score = std::max(score, 800 + static_cast<int>(camera_input.size()));
        score = std::max(score, score_audio_for_camera(a.name, needle));
        if (score > best_score) {
            best_score = score;
            best = a.name;
        }
    }
    // Weak / no association → let AudioInput use substring (camera_input) or default.
    if (best_score < 20)
        return camera_input;
    return best;
}

void apply_camera_audio(MixedCaptureConfig& cfg,
                        bool enable,
                        const std::string& camera_input,
                        float gain) noexcept
{
    cfg.camera_audio = enable;
    cfg.camera_gain = std::clamp(gain, 0.0f, 4.0f);
    if (!enable) {
        cfg.camera_device.clear();
        return;
    }
    cfg.camera_device = resolve_camera_audio_device(camera_input);
}

MixedCaptureConfig make_mixed_capture_config(const std::string& source_token,
                                             const std::string& mic_device,
                                             const std::string& desktop_device,
                                             float mic_gain,
                                             float desktop_gain,
                                             CaptureSource default_source)
{
    MixedCaptureConfig cfg;
    cfg.source = parse_capture_source(source_token, default_source);
    cfg.mic_device = mic_device;
    cfg.desktop_device = desktop_device;
    cfg.mic_gain = mic_gain;
    cfg.desktop_gain = desktop_gain;
    return cfg;
}

MixedCaptureConfig make_recording_capture_config(const std::string& source_token,
                                                 const std::string& mic_device,
                                                 const std::string& desktop_device,
                                                 float mic_gain,
                                                 float desktop_gain,
                                                 CaptureSource default_source)
{
    MixedCaptureConfig cfg = make_mixed_capture_config(
        source_token, mic_device, desktop_device, mic_gain, desktop_gain, default_source);
    cfg.sample_rate = constants::k_recording_sample_rate;
    cfg.channels    = constants::k_recording_channels;
    return cfg;
}

struct MixedAudioCapture::Impl {
    MixedCaptureConfig cfg{};
    std::function<void(const int16_t*, size_t)> on_pcm;
    std::function<void(const int16_t*, size_t, uint32_t, uint32_t)> speech_meter;
    AudioInput mic;
    AudioInput camera;
    DesktopAudioCapture desktop;
    std::mutex mix_mu;
    std::deque<int16_t> desktop_buf;
    std::deque<int16_t> camera_buf;
    std::atomic<bool> running{false};
    bool mic_started = false;
    bool camera_started = false;
    std::string opened_mic;
    std::string opened_desktop;
    std::string opened_camera;
    std::vector<int16_t> scratch;
    uint32_t pcm_sample_rate = constants::k_capture_sample_rate;
    uint32_t pcm_channels    = constants::k_capture_channels;

    size_t side_buf_cap() const
    {
        return static_cast<size_t>(pcm_sample_rate) * pcm_channels;
    }

    void push_desktop(const int16_t* pcm, size_t frames)
    {
        const size_t samples = frames * pcm_channels;
        std::lock_guard<std::mutex> lk(mix_mu);
        push_side_buf(desktop_buf, side_buf_cap(), pcm, samples);
    }

    void push_camera(const int16_t* pcm, size_t frames)
    {
        const size_t samples = frames * pcm_channels;
        std::lock_guard<std::mutex> lk(mix_mu);
        push_side_buf(camera_buf, side_buf_cap(), pcm, samples);
    }

    /// Master clock: sum @p pcm*master_gain with optional desktop/camera side buffers.
    void blend_master(const int16_t* pcm,
                      size_t frames,
                      float master_gain,
                      bool with_desktop,
                      bool with_camera)
    {
        if (!on_pcm || frames == 0) return;
        const size_t samples = frames * pcm_channels;
        scratch.resize(samples);
        std::lock_guard<std::mutex> lk(mix_mu);
        const size_t desk_avail =
            with_desktop ? std::min(samples, desktop_buf.size()) : 0;
        const size_t cam_avail =
            with_camera ? std::min(samples, camera_buf.size()) : 0;
        for (size_t i = 0; i < samples; ++i) {
            float s = static_cast<float>(pcm[i]) * master_gain;
            if (with_desktop)
                s += take_side_sample(desktop_buf, i, desk_avail, cfg.desktop_gain);
            if (with_camera)
                s += take_side_sample(camera_buf, i, cam_avail, cfg.camera_gain);
            scratch[i] = static_cast<int16_t>(std::clamp(s, -32768.0f, 32767.0f));
        }
        if (desk_avail > 0)
            desktop_buf.erase(desktop_buf.begin(),
                              desktop_buf.begin()
                                  + static_cast<std::ptrdiff_t>(desk_avail));
        if (cam_avail > 0)
            camera_buf.erase(camera_buf.begin(),
                             camera_buf.begin()
                                 + static_cast<std::ptrdiff_t>(cam_avail));
        on_pcm(scratch.data(), frames);
    }

    bool start_camera_side(std::string& err)
    {
        try {
            camera.start([this](const int16_t* pcm, size_t frames) {
                push_camera(pcm, frames);
            }, cfg.camera_device, pcm_sample_rate, pcm_channels);
            camera_started = true;
            opened_camera = camera.opened_device_name();
            return true;
        } catch (const std::exception& ex) {
            err = std::string("camera audio: ") + ex.what();
            return false;
        }
    }

    bool start_camera_master(std::string& err)
    {
        const float gain = cfg.camera_gain;
        try {
            auto cam_cb = [this, gain](const int16_t* pcm, size_t frames) {
                if (speech_meter)
                    speech_meter(pcm, frames, pcm_channels, pcm_sample_rate);
                if (gain == 1.0f) {
                    if (on_pcm) on_pcm(pcm, frames);
                    return;
                }
                apply_gain_interleaved(pcm, frames * pcm_channels, gain, scratch);
                if (on_pcm) on_pcm(scratch.data(), frames);
            };
            camera.start(std::move(cam_cb), cfg.camera_device, pcm_sample_rate, pcm_channels);
            camera_started = true;
            opened_camera = camera.opened_device_name();
            return true;
        } catch (const std::exception& ex) {
            err = std::string("camera audio: ") + ex.what();
            return false;
        }
    }
};

MixedAudioCapture::MixedAudioCapture() : m_(std::make_unique<Impl>()) {}
MixedAudioCapture::~MixedAudioCapture() { stop(); }

bool MixedAudioCapture::start(const MixedCaptureConfig& cfg,
                              std::function<void(const int16_t*, size_t)> on_pcm,
                              std::string& err)
{
    stop();
    err.clear();
    if (cfg.source == CaptureSource::none && !cfg.camera_audio) {
        m_->cfg = cfg;
        m_->on_pcm = std::move(on_pcm);
        return true;
    }
    if (!on_pcm) {
        err = "mixed audio: missing PCM callback";
        return false;
    }

    m_->cfg = cfg;
    m_->on_pcm = std::move(on_pcm);
    m_->opened_mic.clear();
    m_->opened_desktop.clear();
    m_->opened_camera.clear();
    m_->mic_started = false;
    m_->camera_started = false;
    resolve_capture_pcm_format(cfg, m_->pcm_sample_rate, m_->pcm_channels);
    {
        std::lock_guard<std::mutex> lk(m_->mix_mu);
        m_->desktop_buf.clear();
        m_->camera_buf.clear();
    }

    const float mic_gain  = cfg.mic_gain;
    const float desk_gain = cfg.desktop_gain;
    const uint32_t rate   = m_->pcm_sample_rate;
    const uint32_t ch     = m_->pcm_channels;
    const bool want_cam   = cfg.camera_audio;

    try {
        // Camera-only (no mic / desktop source).
        if (cfg.source == CaptureSource::none) {
            if (!m_->start_camera_master(err))
                return false;
            m_->running.store(true);
            capture_cancel_add(this);
            return true;
        }

        if (cfg.source == CaptureSource::desktop) {
            if (want_cam && !m_->start_camera_side(err)) {
                m_->camera.stop();
                return false;
            }
            auto desk_cb = [this, desk_gain, want_cam](const int16_t* pcm, size_t frames) {
                if (!want_cam) {
                    if (desk_gain == 1.0f) {
                        if (m_->on_pcm) m_->on_pcm(pcm, frames);
                        return;
                    }
                    apply_gain_interleaved(pcm, frames * m_->pcm_channels, desk_gain, m_->scratch);
                    if (m_->on_pcm) m_->on_pcm(m_->scratch.data(), frames);
                    return;
                }
                m_->blend_master(pcm, frames, desk_gain, /*desktop*/ false, /*camera*/ true);
            };
            if (!m_->desktop.start(desk_cb, cfg.desktop_device, err, rate, ch)) {
                m_->camera.stop();
                m_->camera_started = false;
                return false;
            }
            m_->opened_desktop = m_->desktop.opened_device_name();
            m_->running.store(true);
            capture_cancel_add(this);
            return true;
        }

        if (cfg.source == CaptureSource::mix) {
            if (!m_->desktop.start([this](const int16_t* pcm, size_t frames) {
                    m_->push_desktop(pcm, frames);
                }, cfg.desktop_device, err, rate, ch))
                return false;
            m_->opened_desktop = m_->desktop.opened_device_name();
            if (want_cam && !m_->start_camera_side(err)) {
                m_->desktop.stop();
                return false;
            }
            m_->mic.start([this, want_cam](const int16_t* pcm, size_t frames) {
                if (m_->speech_meter)
                    m_->speech_meter(pcm, frames, m_->pcm_channels, m_->pcm_sample_rate);
                m_->blend_master(pcm, frames, m_->cfg.mic_gain,
                                 /*desktop*/ true, want_cam);
            }, cfg.mic_device, rate, ch);
            m_->mic_started = true;
            m_->opened_mic = m_->mic.opened_device_name();
            m_->running.store(true);
            capture_cancel_add(this);
            return true;
        }

        // mic (± camera)
        if (want_cam && !m_->start_camera_side(err))
            return false;
        if (want_cam) {
            m_->mic.start([this](const int16_t* pcm, size_t frames) {
                if (m_->speech_meter)
                    m_->speech_meter(pcm, frames, m_->pcm_channels, m_->pcm_sample_rate);
                m_->blend_master(pcm, frames, m_->cfg.mic_gain,
                                 /*desktop*/ false, /*camera*/ true);
            }, cfg.mic_device, rate, ch);
        } else {
            auto mic_cb = [this, mic_gain](const int16_t* pcm, size_t frames) {
                if (m_->speech_meter)
                    m_->speech_meter(pcm, frames, m_->pcm_channels, m_->pcm_sample_rate);
                if (mic_gain == 1.0f) {
                    if (m_->on_pcm) m_->on_pcm(pcm, frames);
                    return;
                }
                apply_gain_interleaved(pcm, frames * m_->pcm_channels, mic_gain, m_->scratch);
                if (m_->on_pcm) m_->on_pcm(m_->scratch.data(), frames);
            };
            m_->mic.start(mic_cb, cfg.mic_device, rate, ch);
        }
        m_->mic_started = true;
        m_->opened_mic = m_->mic.opened_device_name();
        m_->running.store(true);
        capture_cancel_add(this);
        return true;
    } catch (const std::exception& ex) {
        m_->desktop.stop();
        m_->mic.stop();
        m_->camera.stop();
        m_->mic_started = false;
        m_->camera_started = false;
        m_->running.store(false);
        err = ex.what();
        return false;
    }
}

void MixedAudioCapture::stop()
{
    capture_cancel_remove(this);
    m_->mic.stop();
    m_->camera.stop();
    m_->desktop.stop();
    m_->mic_started = false;
    m_->camera_started = false;
    m_->running.store(false);
    {
        std::lock_guard<std::mutex> lk(m_->mix_mu);
        m_->desktop_buf.clear();
        m_->camera_buf.clear();
    }
}

bool MixedAudioCapture::is_running() const noexcept
{
    return m_->running.load();
}

const std::string& MixedAudioCapture::opened_mic_device() const noexcept
{
    return m_->opened_mic;
}

const std::string& MixedAudioCapture::opened_desktop_device() const noexcept
{
    return m_->opened_desktop;
}

const std::string& MixedAudioCapture::opened_camera_device() const noexcept
{
    return m_->opened_camera;
}

bool MixedAudioCapture::was_mic_rerouted() const
{
    return m_->mic_started && m_->mic.was_rerouted();
}

uint32_t MixedAudioCapture::pcm_sample_rate() const noexcept
{
    return m_->pcm_sample_rate;
}

uint32_t MixedAudioCapture::pcm_channels() const noexcept
{
    return m_->pcm_channels;
}

void MixedAudioCapture::set_speech_meter(
    std::function<void(const int16_t*, size_t, uint32_t, uint32_t)> fn)
{
    m_->speech_meter = std::move(fn);
}

} // namespace pm::audio

#endif // PM_HAS_AUDIO_IO
