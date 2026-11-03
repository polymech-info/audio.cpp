#include "pm/audio/devices.hpp"
#include "pm/audio/echo.hpp"
#include "pm/audio/log.hpp"
#include "pm/audio/path.hpp"
#include "fs/path_sanitizer.hpp"

#if defined(_MSC_VER)
#  pragma warning(push, 0)
#endif
#include "miniaudio.h"
#if defined(_MSC_VER)
#  pragma warning(pop)
#endif

#include <algorithm>
#include <atomic>
#include <cctype>
#include <chrono>
#include <condition_variable>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <functional>
#include <mutex>
#include <stdexcept>
#include <string>
#include <thread>
#include <vector>

namespace pm::audio {

DevicePrepareFn& device_prepare_slot()
{
    static DevicePrepareFn fn = nullptr;
    return fn;
}

void set_device_prepare(DevicePrepareFn fn)
{
    device_prepare_slot() = fn;
}

void prepare_devices()
{
    if (DevicePrepareFn fn = device_prepare_slot())
        fn();
}


std::vector<DeviceInfo> enumerate_capture_devices()
{
    ma_context ctx{};
    if (ma_context_init(nullptr, 0, nullptr, &ctx) != MA_SUCCESS)
        return {};

    ma_device_info* pCapture  = nullptr;
    ma_uint32       nCapture  = 0;
    ma_device_info* pPlayback = nullptr;
    ma_uint32       nPlayback = 0;

    std::vector<DeviceInfo> result;
    if (ma_context_get_devices(&ctx, &pPlayback, &nPlayback, &pCapture, &nCapture) == MA_SUCCESS) {
        result.reserve(nCapture);
        for (ma_uint32 i = 0; i < nCapture; ++i) {
            DeviceInfo d;
            d.name       = pCapture[i].name;
            d.is_default = (pCapture[i].isDefault != 0);
            if (pCapture[i].nativeDataFormatCount > 0) {
                d.channels    = pCapture[i].nativeDataFormats[0].channels;
                d.sample_rate = pCapture[i].nativeDataFormats[0].sampleRate;
            }
            result.push_back(std::move(d));
        }
    }

    ma_context_uninit(&ctx);
    return result;
}

std::vector<DeviceInfo> enumerate_playback_devices()
{
    ma_context ctx{};
    if (ma_context_init(nullptr, 0, nullptr, &ctx) != MA_SUCCESS)
        return {};

    ma_device_info* pCapture  = nullptr;
    ma_uint32       nCapture  = 0;
    ma_device_info* pPlayback = nullptr;
    ma_uint32       nPlayback = 0;

    std::vector<DeviceInfo> result;
    if (ma_context_get_devices(&ctx, &pPlayback, &nPlayback, &pCapture, &nCapture) == MA_SUCCESS) {
        result.reserve(nPlayback);
        for (ma_uint32 i = 0; i < nPlayback; ++i) {
            DeviceInfo d;
            d.name       = pPlayback[i].name;
            d.is_default = (pPlayback[i].isDefault != 0);
            if (pPlayback[i].nativeDataFormatCount > 0) {
                d.channels    = pPlayback[i].nativeDataFormats[0].channels;
                d.sample_rate = pPlayback[i].nativeDataFormats[0].sampleRate;
            }
            result.push_back(std::move(d));
        }
    }

    ma_context_uninit(&ctx);
    return result;
}

std::vector<DeviceInfo> enumerate_loopback_devices()
{
    ma_context ctx{};
    if (ma_context_init(nullptr, 0, nullptr, &ctx) != MA_SUCCESS)
        return {};

    ma_device_info* pCapture  = nullptr;
    ma_uint32       nCapture  = 0;
    ma_device_info* pPlayback = nullptr;
    ma_uint32       nPlayback = 0;

    std::vector<DeviceInfo> result;
    if (ma_context_get_devices(&ctx, &pPlayback, &nPlayback, &pCapture, &nCapture) == MA_SUCCESS) {
#if defined(_WIN32)
        result.reserve(nPlayback);
        for (ma_uint32 i = 0; i < nPlayback; ++i) {
            DeviceInfo d;
            d.name        = pPlayback[i].name;
            d.is_default  = (pPlayback[i].isDefault != 0);
            d.is_loopback = true;
            if (pPlayback[i].nativeDataFormatCount > 0) {
                d.channels    = pPlayback[i].nativeDataFormats[0].channels;
                d.sample_rate = pPlayback[i].nativeDataFormats[0].sampleRate;
            }
            result.push_back(std::move(d));
        }
#elif !defined(__APPLE__)
        for (ma_uint32 i = 0; i < nCapture; ++i) {
            const std::string name(pCapture[i].name);
            if (name.size() >= 8 && name.compare(name.size() - 8, 8, ".monitor") == 0) {
                DeviceInfo d;
                d.name        = name;
                d.is_default  = (pCapture[i].isDefault != 0);
                d.is_loopback = true;
                if (pCapture[i].nativeDataFormatCount > 0) {
                    d.channels    = pCapture[i].nativeDataFormats[0].channels;
                    d.sample_rate = pCapture[i].nativeDataFormats[0].sampleRate;
                }
                result.push_back(std::move(d));
            }
        }
#endif
    }

    ma_context_uninit(&ctx);
    return result;
}



// ── helpers ──────────────────────────────────────────────────────────────────

static bool icontains(const std::string& haystack, const std::string& needle) {
    if (needle.empty()) return true;
    auto it = std::search(
        haystack.begin(), haystack.end(),
        needle.begin(),   needle.end(),
        [](unsigned char a, unsigned char b) {
            return std::tolower(a) == std::tolower(b);
        });
    return it != haystack.end();
}

// ── AudioInput ────────────────────────────────────────────────────────────────

struct AudioInput::Impl {
    ma_device   device{};
    std::atomic<bool> running{false};
    std::function<void(const int16_t*, size_t)> on_pcm;
    std::string opened_name;
    std::unique_ptr<EchoCancellationProcessor> echo_canceller;
    std::vector<int16_t> echo_scratch;

    // Set to true by the notification callback when the capture device disappears:
    //   • rerouted  → WASAPI/CoreAudio silently switched to a different device (default-device use)
    //   • stopped   → named specific device was unplugged (WASAPI/CoreAudio/ALSA stop the stream)
    // PulseAudio / PipeWire keep the stream alive via transparent routing and may fire neither;
    // audio just keeps flowing on the new device (fallback_to_default behaviour for free).
    // Callers check via AudioInput::was_rerouted(). Resets to false on the next start().
    std::atomic<bool> rerouted{false};

    // Set to true by AudioInput::stop() before calling ma_device_stop() so the notification
    // callback can distinguish a user-initiated stop from an unexpected device removal.
    std::atomic<bool> user_stopping{false};

    static void data_callback(ma_device* pDevice,
                               void*        /*pOutput*/,
                               const void*  pInput,
                               ma_uint32    frameCount)
    {
        auto* self = static_cast<Impl*>(pDevice->pUserData);
        if (self && self->on_pcm && pInput && frameCount > 0) {
            const auto* pcm = static_cast<const int16_t*>(pInput);
            if (self->echo_canceller && self->echo_canceller->active()) {
                self->echo_scratch.assign(pcm, pcm + frameCount);
                self->echo_canceller->process_capture(self->echo_scratch.data(),
                                                      static_cast<size_t>(frameCount));
                pcm = self->echo_scratch.data();
            }
            self->on_pcm(pcm, static_cast<size_t>(frameCount));
        }
    }

    // Called by miniaudio from a notification thread — must NOT call ma_device_stop/uninit/start.
    static void notification_callback(const ma_device_notification* pNotification)
    {
        auto* self = static_cast<Impl*>(pNotification->pDevice->pUserData);
        if (!self) return;

        switch (pNotification->type) {
        case ma_device_notification_type_rerouted:
            // WASAPI/CoreAudio silently switched the stream to a different (default) device.
            // Fires only when the device was opened without a specific pDeviceID.
            self->rerouted.store(true, std::memory_order_release);
            pm::audio::log::trace("[audio] capture device rerouted (default device changed)");
            break;

        case ma_device_notification_type_stopped:
            // Fires when a *named* device is unplugged on WASAPI/CoreAudio/ALSA — the stream
            // can't be rerouted so miniaudio stops it. Ignore if we requested the stop ourselves.
            // Note: PulseAudio/PipeWire keep the stream alive via transparent routing and
            // typically do NOT fire stopped here; the audio keeps flowing on the fallback device.
            if (!self->user_stopping.load(std::memory_order_acquire)) {
                self->rerouted.store(true, std::memory_order_release);
                pm::audio::log::trace("[audio] capture device stopped unexpectedly (device may have been unplugged)");
            }
            break;

        default:
            break;
        }
    }
};

bool capture_device_matches(const std::string& device_name)
{
    if (device_name.empty())
        return true;
    ma_context ctx{};
    if (ma_context_init(nullptr, 0, nullptr, &ctx) != MA_SUCCESS)
        return false;
    ma_device_info* capture = nullptr;
    ma_uint32 n_capture = 0;
    ma_device_info* playback = nullptr;
    ma_uint32 n_playback = 0;
    bool found = false;
    if (ma_context_get_devices(&ctx, &playback, &n_playback,
                               &capture, &n_capture) == MA_SUCCESS) {
        for (ma_uint32 i = 0; i < n_capture; ++i) {
            if (icontains(std::string(capture[i].name), device_name)) {
                found = true;
                break;
            }
        }
    }
    ma_context_uninit(&ctx);
    return found;
}

AudioInput::AudioInput()
    : m_(std::make_unique<Impl>())
{
    prepare_devices();
}
AudioInput::~AudioInput() { stop(); }

void AudioInput::start(std::function<void(const int16_t*, size_t)> on_pcm,
                       const std::string& device_name,
                       uint32_t sample_rate,
                       uint32_t channels,
                       bool apm_ns)
{
    if (m_->running.load())
        stop();

    m_->on_pcm = std::move(on_pcm);

    m_->rerouted.store(false, std::memory_order_relaxed);

    if (sample_rate < 8000) sample_rate = constants::k_capture_sample_rate;
    if (channels < 1) channels = constants::k_capture_channels;
    if (channels > 2) channels = 2;

    ma_device_config cfg = ma_device_config_init(ma_device_type_capture);
    cfg.capture.format       = ma_format_s16;
    cfg.capture.channels     = channels;
    cfg.sampleRate           = sample_rate;
    cfg.dataCallback         = Impl::data_callback;
    cfg.notificationCallback = Impl::notification_callback;
    cfg.pUserData            = m_.get();

    // If a device name was requested, enumerate and find matching id.
    ma_device_id device_id{};
    bool         use_specific = false;
    if (!device_name.empty()) {
        ma_context ctx{};
        if (ma_context_init(nullptr, 0, nullptr, &ctx) == MA_SUCCESS) {
            ma_device_info* pCapture  = nullptr;
            ma_uint32       nCapture  = 0;
            ma_device_info* pPlayback = nullptr;
            ma_uint32       nPlayback = 0;
            if (ma_context_get_devices(&ctx, &pPlayback, &nPlayback,
                                        &pCapture, &nCapture) == MA_SUCCESS) {
                for (ma_uint32 i = 0; i < nCapture; ++i) {
                    if (icontains(std::string(pCapture[i].name), device_name)) {
                        device_id    = pCapture[i].id;
                        use_specific = true;
                        break;
                    }
                }
            }
            ma_context_uninit(&ctx);
        }
        if (!use_specific)
            throw std::runtime_error(
                "audio: no capture device matching \"" + device_name + "\" (see `audio info`)");
    }

    if (use_specific)
        cfg.capture.pDeviceID = &device_id;

    if (sample_rate == constants::k_capture_sample_rate &&
        channels == constants::k_capture_channels) {
        m_->echo_canceller = EchoCancellationProcessor::create({
            static_cast<int>(sample_rate),
            static_cast<int>(channels),
            constants::k_apm_agc,
            apm_ns,
        });
    } else {
        m_->echo_canceller.reset();
    }

    if (ma_device_init(nullptr, &cfg, &m_->device) != MA_SUCCESS)
        throw std::runtime_error("audio: ma_device_init failed");

    if (ma_device_start(&m_->device) != MA_SUCCESS) {
        ma_device_uninit(&m_->device);
        throw std::runtime_error("audio: ma_device_start failed");
    }

    // Store the actual device name reported by miniaudio after init.
    m_->opened_name = m_->device.capture.name;
    if (m_->echo_canceller && m_->echo_canceller->active()) {
        pm::audio::log::info("[audio] echo cancellation active: " + m_->echo_canceller->backend_name());
    }
    m_->running.store(true);
}

void AudioInput::stop() {
    if (!m_->running.load())
        return;
    m_->user_stopping.store(true, std::memory_order_release);
    ma_device_stop(&m_->device);
    ma_device_uninit(&m_->device);
    m_->user_stopping.store(false, std::memory_order_relaxed);
    m_->running.store(false);
    m_->echo_canceller.reset();
    m_->echo_scratch.clear();
}

void AudioInput::process_render_reference(const int16_t* pcm, size_t frame_count)
{
    if (m_->echo_canceller && m_->echo_canceller->active()) {
        m_->echo_canceller->process_render_reference(pcm, frame_count);
    }
}

void AudioInput::set_aec_stream_delay_ms(int ms)
{
    if (m_->echo_canceller && m_->echo_canceller->active()) {
        m_->echo_canceller->set_stream_delay_ms(ms);
    }
}

bool AudioInput::is_running() const {
    return m_->running.load();
}

const std::string& AudioInput::opened_device_name() const {
    return m_->opened_name;
}

bool AudioInput::was_rerouted() const {
    return m_->rerouted.load(std::memory_order_acquire);
}

// ── DesktopAudioCapture ───────────────────────────────────────────────────────

struct DesktopAudioCapture::Impl {
    ma_device   device{};
    std::atomic<bool> running{false};
    std::function<void(const int16_t*, size_t)> on_pcm;
    std::string opened_name;

    static void data_callback(ma_device* pDevice,
                               void*        /*pOutput*/,
                               const void*  pInput,
                               ma_uint32    frameCount)
    {
        auto* self = static_cast<Impl*>(pDevice->pUserData);
        if (self && self->on_pcm && pInput && frameCount > 0)
            self->on_pcm(static_cast<const int16_t*>(pInput),
                         static_cast<size_t>(frameCount));
    }
};

DesktopAudioCapture::DesktopAudioCapture()  : m_(std::make_unique<Impl>()) {}
DesktopAudioCapture::~DesktopAudioCapture() { stop(); }

bool DesktopAudioCapture::start(
    std::function<void(const int16_t* pcm, size_t frame_count)> on_pcm,
    const std::string& device_name,
    std::string& err,
    uint32_t sample_rate,
    uint32_t channels)
{
    if (m_->running.load()) stop();
    m_->on_pcm = std::move(on_pcm);
    if (sample_rate < 8000) sample_rate = constants::k_capture_sample_rate;
    if (channels < 1) channels = constants::k_capture_channels;
    if (channels > 2) channels = 2;

#if defined(__APPLE__)
    err = "desktop audio capture on macOS requires a virtual driver "
          "(BlackHole ≥0.5 or CoreAudio process tap on macOS 14.2+)";
    return false;
#else

#if defined(_WIN32)
    // Windows: WASAPI loopback — targets a render (playback) endpoint.
    ma_device_config cfg = ma_device_config_init(ma_device_type_loopback);
    cfg.capture.format   = ma_format_s16;
    cfg.capture.channels = channels;
    cfg.sampleRate       = sample_rate;
    cfg.dataCallback     = Impl::data_callback;
    cfg.pUserData        = m_.get();

    ma_device_id device_id{};
    bool         use_specific = false;
    if (!device_name.empty()) {
        ma_context ctx{};
        if (ma_context_init(nullptr, 0, nullptr, &ctx) == MA_SUCCESS) {
            ma_device_info* pCapture  = nullptr; ma_uint32 nCapture  = 0;
            ma_device_info* pPlayback = nullptr; ma_uint32 nPlayback = 0;
            if (ma_context_get_devices(&ctx, &pPlayback, &nPlayback,
                                        &pCapture, &nCapture) == MA_SUCCESS) {
                for (ma_uint32 i = 0; i < nPlayback; ++i) {
                    if (icontains(std::string(pPlayback[i].name), device_name)) {
                        device_id    = pPlayback[i].id;
                        use_specific = true;
                        break;
                    }
                }
            }
            ma_context_uninit(&ctx);
        }
        if (!use_specific) {
            err = "audio: no playback device matching \"" + device_name
                + "\" for loopback (see `audio info`)";
            return false;
        }
    }
    if (use_specific)
        cfg.capture.pDeviceID = &device_id;

#else
    // Linux: PulseAudio/PipeWire .monitor capture source.
    // Auto-select the first monitor source when device_name is empty.
    std::string resolved = device_name;
    if (resolved.empty()) {
        auto devs = enumerate_capture_devices();
        for (const auto& d : devs) {
            if (d.name.size() >= 8 && d.name.compare(d.name.size() - 8, 8, ".monitor") == 0) {
                resolved = d.name;
                break;
            }
        }
        if (resolved.empty()) {
            err = "audio: no PulseAudio/PipeWire monitor source found "
                  "(is PulseAudio/PipeWire running? see `audio info`)";
            return false;
        }
    }

    ma_device_config cfg = ma_device_config_init(ma_device_type_capture);
    cfg.capture.format   = ma_format_s16;
    cfg.capture.channels = channels;
    cfg.sampleRate       = sample_rate;
    cfg.dataCallback     = Impl::data_callback;
    cfg.pUserData        = m_.get();

    ma_device_id device_id{};
    bool         use_specific = false;
    {
        ma_context ctx{};
        if (ma_context_init(nullptr, 0, nullptr, &ctx) == MA_SUCCESS) {
            ma_device_info* pCapture  = nullptr; ma_uint32 nCapture  = 0;
            ma_device_info* pPlayback = nullptr; ma_uint32 nPlayback = 0;
            if (ma_context_get_devices(&ctx, &pPlayback, &nPlayback,
                                        &pCapture, &nCapture) == MA_SUCCESS) {
                for (ma_uint32 i = 0; i < nCapture; ++i) {
                    if (icontains(std::string(pCapture[i].name), resolved)) {
                        device_id    = pCapture[i].id;
                        use_specific = true;
                        break;
                    }
                }
            }
            ma_context_uninit(&ctx);
        }
        if (!use_specific) {
            err = "audio: no monitor source matching \"" + resolved
                + "\" (see `audio info`)";
            return false;
        }
    }
    if (use_specific)
        cfg.capture.pDeviceID = &device_id;
#endif

    if (ma_device_init(nullptr, &cfg, &m_->device) != MA_SUCCESS) {
        err = "audio: DesktopAudioCapture ma_device_init failed";
        return false;
    }
    if (ma_device_start(&m_->device) != MA_SUCCESS) {
        ma_device_uninit(&m_->device);
        err = "audio: DesktopAudioCapture ma_device_start failed";
        return false;
    }
    m_->opened_name = m_->device.capture.name;
    m_->running.store(true);
    return true;
#endif // !__APPLE__
}

void DesktopAudioCapture::stop() {
    if (!m_ || !m_->running.exchange(false)) return;
    // Drop the callback before teardown so an in-flight WASAPI period cannot
    // write into a mux that finalize is about to destroy.
    m_->on_pcm = {};
    // ma_device_uninit on WASAPI loopback has been seen to block ~5s (or hang)
    // when the device started but never delivered PCM. Do not pin recorder
    // finalize / XBlox Stop on that — same reason AudioOutput uses a janitor.
    auto dying = std::move(m_);
    m_ = std::make_unique<Impl>();
    std::thread([dying = std::move(dying)]() mutable {
        pm::audio::log::warn("[audio] desktop loopback teardown begin device=\"" +
                     dying->opened_name + "\"");
        ma_device_stop(&dying->device);
        ma_device_uninit(&dying->device);
        pm::audio::log::warn("[audio] desktop loopback teardown done");
    }).detach();
}

bool DesktopAudioCapture::is_running() const { return m_->running.load(); }

const std::string& DesktopAudioCapture::opened_device_name() const {
    return m_->opened_name;
}



// ── Custom IO callbacks for play_ring ─────────────────────────────────────────

namespace {

struct RingDecodeCtx {
    AudioRingBuffer*  ring;
    std::atomic<bool> init_phase{true}; // blocking reads OK during decoder init
};

static ma_result ring_on_read(ma_decoder* d,
                               void* out, size_t want, size_t* got)
{
    auto* ctx = static_cast<RingDecodeCtx*>(d->pUserData);
    if (ctx->init_phase.load(std::memory_order_acquire))
        ctx->ring->wait_for(want); // block on agent thread during format detection
    *got = ctx->ring->pop(out, want);
    if (*got == 0 && ctx->ring->exhausted())
        return MA_AT_END;
    return MA_SUCCESS;
}

static ma_result ring_on_seek(ma_decoder* /*d*/,
                               ma_int64 off, ma_seek_origin origin)
{
    // MP3 streaming: only the trivial no-op seek is supported.
    if (origin == ma_seek_origin_current && off == 0) return MA_SUCCESS;
    return MA_NOT_IMPLEMENTED;
}

static bool ring_head_looks_like_mp3(const uint8_t* head, size_t n) noexcept {
    if (n >= 3 && head[0] == 'I' && head[1] == 'D' && head[2] == '3')
        return true;
    if (n >= 2 && head[0] == 0xFF && (head[1] & 0xE0) == 0xE0)
        return true;
    return false;
}

static std::string ring_non_audio_hint(const AudioRingBuffer& ring) {
    uint8_t head[512];
    const size_t n = ring.peek(head, sizeof(head));
    if (n == 0)
        return "TTS stream was empty";
    if (head[0] == '{' || head[0] == '[') {
        const size_t clip = std::min(n, static_cast<size_t>(240));
        return std::string("TTS API returned non-audio payload: ")
               + std::string(reinterpret_cast<const char*>(head), clip);
    }
    if (n >= 4 && head[0] == 'R' && head[1] == 'I' && head[2] == 'F' && head[3] == 'F')
        return "TTS stream is WAV/RIFF but streaming playback expects MP3";
    if (n >= 4 && head[0] == 'O' && head[1] == 'g' && head[2] == 'g' && head[3] == 'S')
        return "TTS stream is Ogg/Opus but streaming playback expects MP3";
    if (!ring_head_looks_like_mp3(head, n))
        return "TTS stream does not look like MP3 (missing ID3/MPEG frame sync)";
    return {};
}

} // namespace

// ── AudioOutput ───────────────────────────────────────────────────────────────

struct AudioOutput::Impl {
    std::atomic<bool>       stop_flag{false};
    std::atomic<bool>       pause_flag{false};
    std::atomic<bool>       eos{false};
    std::condition_variable cv;
    std::mutex              cv_mu;

    ma_decoder  decoder{};
    ma_device   device{};
    bool        decoder_open  = false;
    bool        device_open   = false;

    // Cached device format — device is reused when the next clip matches.
    ma_format   dev_fmt{ma_format_unknown};
    ma_uint32   dev_ch{0};
    ma_uint32   dev_rate{0};

    // Optional output device name (empty = system default).
    // Set by AudioOutput::set_device_name(); applied on the next play call.
    std::string requested_device_name;
    std::string dev_name; // name used when the device was last opened

    // Set to true by the notification callback when WASAPI reroutes the playback
    // stream to a different device (named device unplugged).  Cleared by ensure_device
    // after it busts the cache so the next play call can recover if the device is back.
    std::atomic<bool> rerouted{false};

    // Called by miniaudio from a notification thread — must NOT call ma_device_stop/uninit/start.
    static void notification_callback(const ma_device_notification* pNotification)
    {
        auto* self = static_cast<Impl*>(pNotification->pDevice->pUserData);
        if (!self) return;

        switch (pNotification->type) {
        case ma_device_notification_type_rerouted:
            // WASAPI/CoreAudio silently switched to a different (default) playback device.
            // Clear the cached name so ensure_device re-enumerates on the next play call —
            // recovery is automatic if the named device is re-plugged by then.
            self->dev_name.clear();
            self->rerouted.store(true, std::memory_order_release);
            pm::audio::log::trace("[audio] playback device rerouted (default device changed)");
            break;

        case ma_device_notification_type_stopped:
            // Named playback device unplugged on WASAPI/CoreAudio/ALSA — stream stopped.
            // Clear dev_name so the next ensure_device call falls back to the system default
            // (or re-opens the named device if it was re-plugged by then).
            // play_sync/play_ring will return shortly because the device stopped mid-stream.
            self->dev_name.clear();
            self->rerouted.store(true, std::memory_order_release);
            pm::audio::log::trace("[audio] playback device stopped"
                          " eos=" + std::to_string(self->eos.load() ? 1 : 0)
                          + " stop_requested=" + std::to_string(self->stop_flag.load() ? 1 : 0));
            break;

        default:
            break;
        }
    }

    // Set during play_ring so that stop() can wake the pre-buffer wait on ring.cv.
    std::atomic<AudioRingBuffer*> active_ring{nullptr};

    // ── Underrun telemetry (lock-free; read by the playback orchestration
    //    thread when play_ring returns to summarise stream health).
    //
    // An underrun = data_callback could not fill the requested frameCount
    // because the decoder ran out of bytes (ring went dry). The user perceives
    // a run of underruns as audio "slowing down" because the silence fill
    // stretches the wall-clock duration of the speech.
    std::atomic<uint32_t> ur_callbacks{0};   // total callback invocations
    std::atomic<uint32_t> ur_underruns{0};   // callbacks that hit short reads
    std::atomic<uint64_t> ur_silent_frames{0}; // total frames replaced w/ silence
    std::atomic<uint64_t> ur_decoded_frames{0}; // frames returned by the decoder

    static void data_callback(ma_device* pDevice,
                               void*        pOutput,
                               const void*  /*pInput*/,
                               ma_uint32    frameCount)
    {
        auto* self = static_cast<Impl*>(pDevice->pUserData);
        if (!self || self->stop_flag.load() || self->pause_flag.load()) {
            std::memset(pOutput, 0,
                frameCount * ma_get_bytes_per_frame(
                    pDevice->playback.format, pDevice->playback.channels));
            return;
        }
        ma_uint64  read = 0;
        ma_result  res  = ma_decoder_read_pcm_frames(&self->decoder,
                                                      pOutput, frameCount, &read);
        self->ur_callbacks.fetch_add(1, std::memory_order_relaxed);
        self->ur_decoded_frames.fetch_add(read, std::memory_order_relaxed);
        if (read < frameCount) {
            const size_t fb = ma_get_bytes_per_frame(pDevice->playback.format,
                                                      pDevice->playback.channels);
            std::memset(static_cast<char*>(pOutput) + read * fb, 0,
                        (frameCount - read) * fb);
            // Only count as an underrun if we're NOT at end-of-stream — at
            // EOS, a short read is expected and silence-fill is the last
            // legitimate frame before the device stops.
            if (res != MA_AT_END) {
                self->ur_underruns.fetch_add(1, std::memory_order_relaxed);
                self->ur_silent_frames.fetch_add(frameCount - read,
                                                  std::memory_order_relaxed);
            }
        }
        // MA_AT_END: correct EOS for both complete buffers and streaming rings.
        // (For complete buffers, MA_AT_END coincides with read < frameCount.)
        if (res == MA_AT_END) {
            self->eos.store(true);
            self->cv.notify_one();
        }
    }

    // Open or reuse the WASAPI device for the given format triple.
    // Returns false and leaves device_open unchanged on failure.
    bool ensure_device(ma_format fmt, ma_uint32 ch, ma_uint32 rate) {
        const bool match = device_open
            && fmt == dev_fmt && ch == dev_ch && rate == dev_rate
            && requested_device_name == dev_name;
        if (match) return true;

        if (device_open) {
            ma_device_stop(&device);
            ma_device_uninit(&device);
            device_open = false;
        }
        rerouted.store(false, std::memory_order_relaxed); // clear before re-init; notification fires fresh

        ma_device_config cfg = ma_device_config_init(ma_device_type_playback);
        cfg.playback.format      = fmt;
        cfg.playback.channels    = ch;
        cfg.sampleRate           = rate;
        cfg.dataCallback         = data_callback;
        cfg.notificationCallback = notification_callback;
        cfg.pUserData            = this;

        // Named device selection: enumerate, find by substring, copy device id.
        // If the device is absent (unplugged), cfg.playback.pDeviceID stays nullptr and
        // miniaudio opens the system default — graceful fallback on all platforms.
        ma_device_id named_id{};
        bool         found_named = false;
        if (!requested_device_name.empty()) {
            ma_context enum_ctx{};
            if (ma_context_init(nullptr, 0, nullptr, &enum_ctx) == MA_SUCCESS) {
                ma_device_info* pPlayback = nullptr;
                ma_uint32       nPlayback = 0;
                ma_device_info* pCapture  = nullptr;
                ma_uint32       nCapture  = 0;
                if (ma_context_get_devices(&enum_ctx, &pPlayback, &nPlayback,
                                           &pCapture,  &nCapture)  == MA_SUCCESS) {
                    for (ma_uint32 i = 0; i < nPlayback; ++i) {
                        if (icontains(std::string(pPlayback[i].name), requested_device_name)) {
                            named_id = pPlayback[i].id;
                            cfg.playback.pDeviceID = &named_id;
                            found_named = true;
                            break;
                        }
                    }
                }
                ma_context_uninit(&enum_ctx);
            }
            if (!found_named)
                pm::audio::log::trace(("[audio] playback device \"" + requested_device_name
                               + "\" not found — falling back to system default").c_str());
        }

        if (ma_device_init(nullptr, &cfg, &device) != MA_SUCCESS)
            return false;
        device_open = true;
        dev_fmt  = fmt;
        dev_ch   = ch;
        dev_rate = rate;
        dev_name = requested_device_name;
        return true;
    }

    // Common wait loop used by both play_sync and play_ring.
    void wait_until_done() {
        // Polling fallback covers the SIGINT signal-handler path where
        // stop() may only set stop_flag atomically without calling notify_one().
        std::unique_lock<std::mutex> lk(cv_mu);
        while (!eos.load() && !stop_flag.load())
            cv.wait_for(lk, std::chrono::milliseconds(constants::k_stop_poll_ms));
    }
};

void AudioOutput::set_device_name(const std::string& name) {
    impl_->requested_device_name = name;
}

// ── Asynchronous device teardown ──────────────────────────────────────────────
//
// ma_device_uninit on WASAPI has been observed to hang indefinitely (one such
// trip is logged with `~AudioOutput ma_device_uninit` and never a matching
// `~AudioOutput exit`). When the destructor was called from a worker thread's
// TLS dtor at thread exit, the janitor.join() blocked too — leaking one zombie
// thread per cancelled TTS until process exit.
//
// Fix: ~AudioOutput moves `impl_` into a process-wide queue and a single
// dedicated worker drains it serially with stop/uninit. The dying thread
// returns immediately; the janitor returns immediately; the audio device gets
// reclaimed (or, in the worst case, the teardown worker hangs but no other
// thread is blocked behind it).
//
// The worker is started from AudioOutput's constructor, not the destructor.
// thread_local AudioOutput (TTS) runs ~AudioOutput from a CRT TLS callback
// under the loader lock; _beginthreadex fails there, std::thread throws
// system_error, and a noexcept wrapper would std::terminate the process.
namespace {

std::mutex& teardown_mu() {
    static std::mutex m;
    return m;
}
std::condition_variable& teardown_cv() {
    static std::condition_variable cv;
    return cv;
}
std::vector<std::unique_ptr<AudioOutput::Impl>>& teardown_queue() {
    static std::vector<std::unique_ptr<AudioOutput::Impl>> q;
    return q;
}
std::atomic<bool> g_teardown_alive{false};

void teardown_worker_loop() {
    for (;;) {
        std::unique_ptr<AudioOutput::Impl> task;
        {
            std::unique_lock<std::mutex> lk(teardown_mu());
            teardown_cv().wait(lk, [] { return !teardown_queue().empty(); });
            task = std::move(teardown_queue().front());
            teardown_queue().erase(teardown_queue().begin());
        }
        if (!task) continue;
        if (task->device_open) {
            pm::audio::log::trace("[cancel] (teardown) ma_device_stop");
            ma_device_stop(&task->device);
            pm::audio::log::trace("[cancel] (teardown) ma_device_uninit");
            ma_device_uninit(&task->device);
            task->device_open = false;
            pm::audio::log::trace("[cancel] (teardown) ma_device_uninit done");
        }
        if (task->decoder_open) {
            ma_decoder_uninit(&task->decoder);
            task->decoder_open = false;
        }
        // task unique_ptr goes out of scope, Impl is freed.
    }
}

void start_teardown_worker_once() noexcept {
    if (g_teardown_alive.load(std::memory_order_acquire))
        return;
    std::lock_guard<std::mutex> lk(teardown_mu());
    if (g_teardown_alive.load(std::memory_order_relaxed))
        return;
    pm::audio::log::trace("[cancel] (teardown) worker thread starting");
    try {
        std::thread(teardown_worker_loop).detach();
        g_teardown_alive.store(true, std::memory_order_release);
    } catch (...) {
        // _beginthreadex failed (typical from TLS dtor / process shutdown).
        // Leave the flag clear so a later ctor on a live thread can retry.
        pm::audio::log::warn("[cancel] (teardown) worker thread start failed");
    }
}

void enqueue_device_teardown(std::unique_ptr<AudioOutput::Impl> impl) noexcept {
    std::size_t depth = 0;
    {
        std::lock_guard<std::mutex> lk(teardown_mu());
        teardown_queue().push_back(std::move(impl));
        depth = teardown_queue().size();
    }
    start_teardown_worker_once();
    teardown_cv().notify_one();
    // depth > 1 means a prior uninit is still running (likely a WASAPI hang).
    // It's harmless — devices queue up — but worth flagging once in trace so
    // it surfaces in support logs.
    if (depth > 1)
        pm::audio::log::trace("[cancel] (teardown) queue depth=" + std::to_string(depth)
                      + " (prior uninit still running)");
}

} // namespace

AudioOutput::AudioOutput() : impl_(std::make_unique<Impl>()) {
    // Spawn the teardown worker while this thread is fully alive. Must not
    // wait until ~AudioOutput — that often runs from thread_local destruction.
    start_teardown_worker_once();
}

AudioOutput::~AudioOutput() {
    stop();
    // Hand the device off to the teardown worker. impl_ becomes nullptr; that's
    // fine because no other methods on this AudioOutput will run after dtor.
    if (impl_ && (impl_->device_open || impl_->decoder_open)) {
        enqueue_device_teardown(std::move(impl_));
    }    
}

void AudioOutput::play_sync(const uint8_t* data, size_t bytes) {
    impl_->pause_flag.store(false);
    impl_->stop_flag.store(false);
    impl_->eos.store(false);
    impl_->ur_callbacks.store(0);
    impl_->ur_underruns.store(0);
    impl_->ur_silent_frames.store(0);
    impl_->ur_decoded_frames.store(0);

    // Reinit decoder for this clip (always; format may differ from last call).
    if (impl_->decoder_open) {
        ma_decoder_uninit(&impl_->decoder);
        impl_->decoder_open = false;
    }
    if (ma_decoder_init_memory(data, bytes, nullptr, &impl_->decoder) != MA_SUCCESS)
        throw std::runtime_error("AudioOutput: ma_decoder_init_memory failed");
    impl_->decoder_open = true;

    // Reuse WASAPI device if format unchanged — skips the 100–300 ms init cost.
    if (!impl_->ensure_device(impl_->decoder.outputFormat,
                               impl_->decoder.outputChannels,
                               impl_->decoder.outputSampleRate)) {
        ma_decoder_uninit(&impl_->decoder);
        impl_->decoder_open = false;
        throw std::runtime_error("AudioOutput: ma_device_init failed");
    }

    if (ma_device_start(&impl_->device) != MA_SUCCESS)
        throw std::runtime_error("AudioOutput: ma_device_start failed");

    const auto playback_started = std::chrono::steady_clock::now();
    constexpr auto k_playback_trace_interval = std::chrono::seconds(3);
    {
        std::unique_lock<std::mutex> lk(impl_->cv_mu);
        while (!impl_->eos.load() && !impl_->stop_flag.load()) {
            const bool finished = impl_->cv.wait_for(lk, k_playback_trace_interval, [&] {
                return impl_->eos.load() || impl_->stop_flag.load();
            });
            if (finished) break;

            const uint64_t decoded_frames = impl_->ur_decoded_frames.load();
            const uint64_t audio_ms = impl_->dev_rate > 0
                ? (decoded_frames * 1000ULL / impl_->dev_rate)
                : 0;
            const auto wall_ms = std::chrono::duration_cast<std::chrono::milliseconds>(
                std::chrono::steady_clock::now() - playback_started).count();     
        }
    }

    const uint64_t decoded_frames = impl_->ur_decoded_frames.load();
    const uint64_t audio_ms = impl_->dev_rate > 0
        ? (decoded_frames * 1000ULL / impl_->dev_rate)
        : 0;
    const auto wall_ms = std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::steady_clock::now() - playback_started).count();
    

    if (!impl_->stop_flag.load())
        std::this_thread::sleep_for(std::chrono::milliseconds(constants::k_drain_ms));

    if (!impl_->stop_flag.load() && decoded_frames == 0) {
        ma_device_stop(&impl_->device);
        ma_decoder_uninit(&impl_->decoder);
        impl_->decoder_open = false;
        throw std::runtime_error("AudioOutput: playback produced no audio frames");
    }

    ma_device_stop(&impl_->device);
    // Decoder is uninited here; device is kept alive for the next call.
    ma_decoder_uninit(&impl_->decoder);
    impl_->decoder_open = false;
}

void AudioOutput::play_ring(AudioRingBuffer& ring, int encoding) {
    impl_->pause_flag.store(false);
    impl_->stop_flag.store(false);
    impl_->eos.store(false);
    impl_->ur_callbacks.store(0);
    impl_->ur_underruns.store(0);
    impl_->ur_silent_frames.store(0);
    impl_->ur_decoded_frames.store(0);

    // Timing: break the path between play_ring entry and first audio sample
    // into measurable phases so we can spot which one creeps if "drag" returns.
    const auto t_entry = std::chrono::steady_clock::now();
    auto ms_since = [](std::chrono::steady_clock::time_point t0) {
        return std::chrono::duration_cast<std::chrono::milliseconds>(
            std::chrono::steady_clock::now() - t0).count();
    };

    // Pre-buffer: wait for enough bytes for MP3 frame detection + initial audio.
    // active_ring lets stop() reach ring.cv.notify_all() so the wait exits
    // immediately when abort_active_speak() / SIGINT fires.
    struct ActiveRingGuard {
        Impl*             impl;
        AudioRingBuffer*  ring;
        ActiveRingGuard(Impl* i, AudioRingBuffer* r) : impl(i), ring(r) {
            impl->active_ring.store(ring, std::memory_order_release);
        }
        ~ActiveRingGuard() {
            impl->active_ring.store(nullptr, std::memory_order_release);
        }
    } ring_guard(impl_.get(), &ring);
    {
        std::unique_lock<std::mutex> lk(ring.mu);
        ring.cv.wait(lk, [&] {
            return ring.readable() >= constants::k_ring_prebuf
                || ring.done.load()
                || ring.cancelled.load()
                || impl_->stop_flag.load();
        });
    }
    const auto t_prebuf = std::chrono::steady_clock::now();
    if (impl_->stop_flag.load() || ring.cancelled.load()) {
        return;
    }

    if (ring.exhausted() && ring.readable() == 0)
        throw std::runtime_error("AudioOutput: ring closed before any data");

    if (const std::string hint = ring_non_audio_hint(ring); !hint.empty())
        throw std::runtime_error("AudioOutput: " + hint);

    // Init decoder from the ring buffer with the explicit encoding format so
    // miniaudio skips format probing that requires backward seeks.
    RingDecodeCtx rctx{&ring};
    ma_decoder_config dcfg = ma_decoder_config_init_default();
    dcfg.encodingFormat = static_cast<ma_encoding_format>(encoding);

    if (impl_->decoder_open) {
        ma_decoder_uninit(&impl_->decoder);
        impl_->decoder_open = false;
    }
    if (ma_decoder_init(ring_on_read, ring_on_seek,
                        &rctx, &dcfg, &impl_->decoder) != MA_SUCCESS) {
        std::string msg = "AudioOutput: ring decoder init failed";
        if (const std::string hint = ring_non_audio_hint(ring); !hint.empty())
            msg += " (" + hint + ")";
        else
            msg += " (bytes=" + std::to_string(ring.readable()) + ")";
        throw std::runtime_error(msg);
    }
    impl_->decoder_open = true;

    // Switch to non-blocking reads for the audio callback thread.
    rctx.init_phase.store(false, std::memory_order_release);

    const bool device_was_open = impl_->device_open
        && impl_->dev_fmt  == impl_->decoder.outputFormat
        && impl_->dev_ch   == impl_->decoder.outputChannels
        && impl_->dev_rate == impl_->decoder.outputSampleRate;

    if (!impl_->ensure_device(impl_->decoder.outputFormat,
                               impl_->decoder.outputChannels,
                               impl_->decoder.outputSampleRate)) {
        ma_decoder_uninit(&impl_->decoder);
        impl_->decoder_open = false;
        throw std::runtime_error("AudioOutput: ma_device_init failed");
    }
    const auto t_device = std::chrono::steady_clock::now();

    if (ma_device_start(&impl_->device) != MA_SUCCESS)
        throw std::runtime_error("AudioOutput: ma_device_start failed");
    const auto t_start = std::chrono::steady_clock::now();

    const auto prebuf_ms = std::chrono::duration_cast<std::chrono::milliseconds>(
                             t_prebuf - t_entry).count();
    const auto decode_ms = std::chrono::duration_cast<std::chrono::milliseconds>(
                             t_device - t_prebuf).count();
    const auto start_ms  = std::chrono::duration_cast<std::chrono::milliseconds>(
                             t_start - t_device).count();
    // Sample stream health off the real-time audio callback. Logging from the
    // callback itself can block it and manufacture the timing problem we are
    // trying to diagnose.
    constexpr auto k_buffer_trace_interval = std::chrono::seconds(3);
    uint64_t previous_written = ring.wpos.load(std::memory_order_acquire);
    uint64_t previous_read    = ring.rpos.load(std::memory_order_acquire);
    {
        std::unique_lock<std::mutex> lk(impl_->cv_mu);
        while (!impl_->eos.load() && !impl_->stop_flag.load() && !ring.cancelled.load()) {
            const bool finished = impl_->cv.wait_for(lk, k_buffer_trace_interval, [&] {
                return impl_->eos.load() || impl_->stop_flag.load() || ring.cancelled.load();
            });
            if (finished) break;
            if (ring.cancelled.load()) {
                impl_->stop_flag.store(true, std::memory_order_release);
                break;
            }

            const uint64_t written = ring.wpos.load(std::memory_order_acquire);
            const uint64_t read    = ring.rpos.load(std::memory_order_acquire);
            const uint64_t queued  = written - read;
            previous_written = written;
            previous_read    = read;
        }
    }

    if (!impl_->stop_flag.load())
        std::this_thread::sleep_for(std::chrono::milliseconds(constants::k_drain_ms));

    ma_device_stop(&impl_->device);
    ma_decoder_uninit(&impl_->decoder);
    impl_->decoder_open = false;
    // rctx lives on the stack until here; ma_device_stop ensures the
    // audio callback has returned before we destroy it. ✓

    // Underrun summary — surfaces "audio slowing down" / "gaps" issues.
    // If silent_frames is non-trivial relative to sample_rate, the user
    // perceives the speech as longer than intended.
    const auto callbacks     = impl_->ur_callbacks.load();
    const auto underruns     = impl_->ur_underruns.load();
    const auto silent_frames = impl_->ur_silent_frames.load();
    if (underruns > 0) {
        const double silent_ms = impl_->dev_rate > 0
            ? (1000.0 * static_cast<double>(silent_frames) / impl_->dev_rate)
            : 0.0;
        const double pct = callbacks > 0
            ? (100.0 * static_cast<double>(underruns) / callbacks)
            : 0.0;
    }
}

void AudioOutput::pause() {
    impl_->pause_flag.store(true);
}

void AudioOutput::resume() {
    impl_->pause_flag.store(false);
}

void AudioOutput::stop() {
    impl_->pause_flag.store(false);
    impl_->stop_flag.store(true);
    impl_->cv.notify_one();
    // Wake play_ring waits on ring.cv (pre-buffer or if download is cancelled).
    auto* ring = impl_->active_ring.load(std::memory_order_acquire);
    if (ring) {
        ring->cancel();
        ring->cv.notify_all();
    }
}

bool AudioOutput::is_playing() const {
    return impl_->device_open && !impl_->eos.load();
}

// ── Async file playback ───────────────────────────────────────────────────────

namespace {

// Shared AudioOutput for async file playback.
// shared_ptr lets stop_sound_file() hold a ref and call stop() without
// holding the mutex during the entire play_sync() call.
std::shared_ptr<AudioOutput> g_async_player;
std::mutex                    g_async_player_mtx;
std::atomic<bool>             g_async_playback_active{false};

} // namespace

void play_sound_file_async(const std::string& file_path) {
    // Stop any existing playback first.
    stop_sound_file();

    // Resolve relative paths against current working directory.
    namespace fs = std::filesystem;
    fs::path path = media::path::path_from_utf8(file_path);
    if (path.is_relative()) {
        path = fs::absolute(path);
    }

    // Read the entire file into memory.
    std::ifstream ifs(path, std::ios::binary | std::ios::ate);
    if (!ifs) {
        throw std::runtime_error("play_sound_file: cannot open: " + media::path::path_to_utf8(path));
    }

    const auto size = ifs.tellg();
    if (size <= 0) {
        throw std::runtime_error("play_sound_file: empty file: " + media::path::path_to_utf8(path));
    }

    ifs.seekg(0, std::ios::beg);
    std::vector<uint8_t> buffer(static_cast<size_t>(size));
    if (!ifs.read(reinterpret_cast<char*>(buffer.data()), size)) {
        throw std::runtime_error("play_sound_file: read error: " + media::path::path_to_utf8(path));
    }
    ifs.close();

    // Create the player and mark active before launching the thread so that
    // a rapid stop_sound_file() call can always reach the right instance.
    std::shared_ptr<AudioOutput> player;
    {
        std::lock_guard<std::mutex> lk(g_async_player_mtx);
        g_async_player = std::make_shared<AudioOutput>();
        player = g_async_player;
        g_async_playback_active.store(true);
    }

    // Start playback on a detached thread. The thread shares ownership of
    // the player so it stays alive even if g_async_player is replaced by a
    // later call. The mutex is NOT held during play_sync so stop_sound_file()
    // can always call stop() without stalling.
    std::thread([buf = std::move(buffer), player]() mutable {
        try {
            player->play_sync(buf.data(), buf.size());
        } catch (...) {}

        std::lock_guard<std::mutex> lk(g_async_player_mtx);
        g_async_playback_active.store(false);
        // Only reset the global if it still points to our player; a later
        // play_sound_file_async() call may have already swapped it out.
        if (g_async_player.get() == player.get())
            g_async_player.reset();
    }).detach();
}

void play_sound_file_sync(const std::string& file_path,
                          std::function<bool()> cancel_fn)
{
    // Reuse the same shared g_async_player so repeated calls on the same
    // AudioOutput instance benefit from WASAPI device reuse (no re-init cost).
    stop_sound_file(); // stop any previous playback

    namespace fs = std::filesystem;
    fs::path path = media::path::path_from_utf8(file_path);
    if (path.is_relative())
        path = fs::absolute(path);

    std::ifstream ifs(path, std::ios::binary | std::ios::ate);
    if (!ifs) throw std::runtime_error("play_sound_file_sync: cannot open: " + media::path::path_to_utf8(path));
    const auto sz = ifs.tellg();
    if (sz <= 0) throw std::runtime_error("play_sound_file_sync: empty file: " + media::path::path_to_utf8(path));
    ifs.seekg(0, std::ios::beg);
    std::vector<uint8_t> buf(static_cast<size_t>(sz));
    if (!ifs.read(reinterpret_cast<char*>(buf.data()), sz))
        throw std::runtime_error("play_sound_file_sync: read error: " + media::path::path_to_utf8(path));
    ifs.close();

    std::shared_ptr<AudioOutput> player;
    {
        std::lock_guard<std::mutex> lk(g_async_player_mtx);
        if (!g_async_player)
            g_async_player = std::make_shared<AudioOutput>();
        player = g_async_player;
        g_async_playback_active.store(true);
    }

    // Run a cancel-polling thread while play_sync blocks on this thread.
    std::atomic<bool> play_done{false};
    std::thread watcher([&]() {
        while (!play_done.load()) {
            std::this_thread::sleep_for(std::chrono::milliseconds(80));
            if (cancel_fn && cancel_fn()) {
                player->stop();
                return;
            }
        }
    });

    try {
        player->play_sync(buf.data(), buf.size());
    } catch (...) {
        play_done.store(true);
        if (watcher.joinable()) watcher.join();
        std::lock_guard<std::mutex> lk(g_async_player_mtx);
        g_async_playback_active.store(false);
        throw;
    }

    play_done.store(true);
    if (watcher.joinable()) watcher.join();

    std::lock_guard<std::mutex> lk(g_async_player_mtx);
    g_async_playback_active.store(false);
}

void stop_sound_file() {
    // Grab a shared reference so the player stays alive after we release the lock.
    std::shared_ptr<AudioOutput> to_stop;
    {
        std::lock_guard<std::mutex> lk(g_async_player_mtx);
        to_stop = g_async_player;
    }
    // Signal stop outside the mutex so the playback thread (which is no longer
    // holding the mutex during play_sync) can observe the flag and unblock.
    if (to_stop)
        to_stop->stop();

    // Wait briefly for the playback thread to finish tearing down.
    for (int i = 0; i < 50 && g_async_playback_active.load(); ++i)
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
}

} // namespace pm::audio

