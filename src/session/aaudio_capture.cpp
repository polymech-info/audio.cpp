#include "pm/audio/session/aaudio_capture.hpp"

#if !defined(__ANDROID__)

namespace pm::audio::session {

bool aaudio_available() { return false; }

AAudioStatus aaudio_unavailable()
{
    AAudioStatus status;
    status.reason = "AAudio capture is Android-only";
    return status;
}

AAudioStatus aaudio_capture_start(std::uint64_t, int, bool, const char*, int) { return aaudio_unavailable(); }
AAudioStatus aaudio_capture_stop() { return aaudio_unavailable(); }
AAudioStatus aaudio_capture_stop_wav(const std::string&) { return aaudio_unavailable(); }
AAudioStatus aaudio_capture_status() { return aaudio_unavailable(); }
AAudioStatus aaudio_play_tone(std::uint64_t, int, bool, int) { return aaudio_unavailable(); }
std::size_t aaudio_capture_read(std::int16_t*, std::size_t, int* rate) {
    if (rate) *rate = 0;
    return 0;
}

} // namespace pm::audio::session

#else

#include <aaudio/AAudio.h>

#include "pm/audio/session/audio_graph.hpp"

#include <android/log.h>
#include <dlfcn.h>

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <cstring>
#include <fstream>
#include <memory>
#include <mutex>
#include <vector>

namespace pm::audio::session {
namespace {

struct Api {
    bool tried = false;
    bool ok = false;
    void* lib = nullptr;
    aaudio_result_t (*create)(AAudioStreamBuilder**) = nullptr;
    void (*destroy)(AAudioStreamBuilder*) = nullptr;
    void (*setDirection)(AAudioStreamBuilder*, aaudio_direction_t) = nullptr;
    void (*setDeviceId)(AAudioStreamBuilder*, int32_t) = nullptr;
    void (*setSharingMode)(AAudioStreamBuilder*, aaudio_sharing_mode_t) = nullptr;
    void (*setFormat)(AAudioStreamBuilder*, aaudio_format_t) = nullptr;
    void (*setChannelCount)(AAudioStreamBuilder*, int32_t) = nullptr;
    void (*setSampleRate)(AAudioStreamBuilder*, int32_t) = nullptr;
    void (*setInputPreset)(AAudioStreamBuilder*, aaudio_input_preset_t) = nullptr;
    void (*setPerformanceMode)(AAudioStreamBuilder*, aaudio_performance_mode_t) = nullptr;
    void (*setBufferCapacity)(AAudioStreamBuilder*, int32_t) = nullptr;
    void (*setUsage)(AAudioStreamBuilder*, aaudio_usage_t) = nullptr;
    void (*setContentType)(AAudioStreamBuilder*, aaudio_content_type_t) = nullptr;
    void (*setDataCallback)(AAudioStreamBuilder*, AAudioStream_dataCallback, void*) = nullptr;
    void (*setErrorCallback)(AAudioStreamBuilder*, AAudioStream_errorCallback, void*) = nullptr;
    aaudio_result_t (*open)(AAudioStreamBuilder*, AAudioStream**) = nullptr;
    aaudio_result_t (*requestStart)(AAudioStream*) = nullptr;
    aaudio_result_t (*requestStop)(AAudioStream*) = nullptr;
    aaudio_result_t (*close)(AAudioStream*) = nullptr;
    int32_t (*getDeviceId)(AAudioStream*) = nullptr;
    int32_t (*getSampleRate)(AAudioStream*) = nullptr;
};

Api& api()
{
    static Api a;
    return a;
}

std::mutex& api_mu()
{
    static std::mutex mu;
    return mu;
}

template <class T>
T load(void* lib, const char* name)
{
    return reinterpret_cast<T>(dlsym(lib, name));
}

bool load_api()
{
    std::lock_guard<std::mutex> lock(api_mu());
    Api& a = api();
    if (a.tried)
        return a.ok;
    a.tried = true;
    a.lib = dlopen("libaaudio.so", RTLD_NOW);
    if (!a.lib)
        return false;
    a.create = load<decltype(a.create)>(a.lib, "AAudio_createStreamBuilder");
    a.destroy = load<decltype(a.destroy)>(a.lib, "AAudioStreamBuilder_delete");
    a.setDirection = load<decltype(a.setDirection)>(a.lib, "AAudioStreamBuilder_setDirection");
    a.setDeviceId = load<decltype(a.setDeviceId)>(a.lib, "AAudioStreamBuilder_setDeviceId");
    a.setSharingMode = load<decltype(a.setSharingMode)>(a.lib, "AAudioStreamBuilder_setSharingMode");
    a.setFormat = load<decltype(a.setFormat)>(a.lib, "AAudioStreamBuilder_setFormat");
    a.setChannelCount = load<decltype(a.setChannelCount)>(a.lib, "AAudioStreamBuilder_setChannelCount");
    a.setSampleRate = load<decltype(a.setSampleRate)>(a.lib, "AAudioStreamBuilder_setSampleRate");
    a.setInputPreset = load<decltype(a.setInputPreset)>(a.lib, "AAudioStreamBuilder_setInputPreset");
    a.setPerformanceMode = load<decltype(a.setPerformanceMode)>(a.lib, "AAudioStreamBuilder_setPerformanceMode");
    a.setBufferCapacity = load<decltype(a.setBufferCapacity)>(a.lib, "AAudioStreamBuilder_setBufferCapacityInFrames");
    a.setUsage = load<decltype(a.setUsage)>(a.lib, "AAudioStreamBuilder_setUsage");
    a.setContentType = load<decltype(a.setContentType)>(a.lib, "AAudioStreamBuilder_setContentType");
    a.setDataCallback = load<decltype(a.setDataCallback)>(a.lib, "AAudioStreamBuilder_setDataCallback");
    a.setErrorCallback = load<decltype(a.setErrorCallback)>(a.lib, "AAudioStreamBuilder_setErrorCallback");
    a.open = load<decltype(a.open)>(a.lib, "AAudioStreamBuilder_openStream");
    a.requestStart = load<decltype(a.requestStart)>(a.lib, "AAudioStream_requestStart");
    a.requestStop = load<decltype(a.requestStop)>(a.lib, "AAudioStream_requestStop");
    a.close = load<decltype(a.close)>(a.lib, "AAudioStream_close");
    a.getDeviceId = load<decltype(a.getDeviceId)>(a.lib, "AAudioStream_getDeviceId");
    a.getSampleRate = load<decltype(a.getSampleRate)>(a.lib, "AAudioStream_getSampleRate");
    a.ok = a.create && a.destroy && a.setDirection && a.setDeviceId && a.setSharingMode && a.setFormat
        && a.setChannelCount && a.setSampleRate && a.setDataCallback && a.setErrorCallback
        && a.open && a.requestStart && a.requestStop && a.close && a.getDeviceId;
    return a.ok;
}

struct Stream {
    std::mutex mu;
    std::condition_variable cv;
    AAudioStream* stream = nullptr;
    std::atomic<std::int64_t> frames{0};
    // Peak envelope, 0..32767. The ring stays intact for the WAV.
    std::atomic<int> envelope{0};
    std::atomic<bool> lost{false};
    std::atomic<int> error_code{0};
    std::atomic<bool> running{false};
    std::atomic<bool> finished{false};
    int opened_id = -1;
    int target_frames = 0;
    int sample_rate = 48000;
    std::atomic<pm::audio::graph::SpscRing*> pcm{nullptr};
    std::unique_ptr<pm::audio::graph::SpscRing> pcm_store;
    std::atomic<std::uint64_t> granted_token{0};
};

Stream& capture_stream()
{
    static Stream s;
    return s;
}

Stream& playback_stream()
{
    static Stream s;
    return s;
}

void close_locked(Stream& s)
{
    Api& a = api();
    if (s.stream && a.requestStop && a.close) {
        a.requestStop(s.stream);
        a.close(s.stream);
    }
    s.stream = nullptr;
    s.running.store(false);
    s.pcm.store(nullptr, std::memory_order_release);
    s.granted_token.store(0, std::memory_order_release);
}

aaudio_data_callback_result_t capture_data(AAudioStream*, void*, void* audioData, int32_t numFrames)
{
    Stream& s = capture_stream();
    if (s.lost.load(std::memory_order_acquire))
        return AAUDIO_CALLBACK_RESULT_STOP;
    const std::uint64_t granted = s.granted_token.load(std::memory_order_acquire);
    if (granted == 0 || granted != lease_token()) {
        s.lost.store(true, std::memory_order_release);
        return AAUDIO_CALLBACK_RESULT_STOP;
    }
    if (numFrames > 0) {
        auto* ring = s.pcm.load(std::memory_order_acquire);
        if (ring && audioData)
            ring->write_or_drop(static_cast<const std::int16_t*>(audioData),
                                static_cast<std::size_t>(numFrames));
        if (audioData) {
            const auto* samples = static_cast<const std::int16_t*>(audioData);
            int burst = 0;
            for (int32_t i = 0; i < numFrames; ++i) {
                int mag = samples[i];
                if (mag < 0)
                    mag = -mag;
                if (mag > burst)
                    burst = mag;
            }
            const int prev = s.envelope.load(std::memory_order_relaxed);
            const int decayed = prev - prev / 8;
            s.envelope.store(burst > decayed ? burst : decayed, std::memory_order_relaxed);
        }
        s.frames.fetch_add(numFrames, std::memory_order_relaxed);
    }
    return AAUDIO_CALLBACK_RESULT_CONTINUE;
}

void capture_error(AAudioStream*, void*, aaudio_result_t error)
{
    // A disconnect must not reopen the default device. Closing happens on stop().
    Stream& s = capture_stream();
    s.error_code.store(static_cast<int>(error), std::memory_order_release);
    s.lost.store(true, std::memory_order_release);
    __android_log_print(ANDROID_LOG_ERROR, "TanitHandsFree",
        "aaudio error=%d", static_cast<int>(error));
}

aaudio_data_callback_result_t tone_data(AAudioStream*, void*, void* audioData, int32_t numFrames)
{
    Stream& s = playback_stream();
    const std::uint64_t granted = s.granted_token.load(std::memory_order_acquire);
    if (granted == 0 || granted != lease_token()) {
        s.lost.store(true, std::memory_order_release);
        s.finished.store(true, std::memory_order_release);
        s.cv.notify_all();
        return AAUDIO_CALLBACK_RESULT_STOP;
    }
    auto* dst = static_cast<std::int16_t*>(audioData);
    const std::int64_t start = s.frames.load(std::memory_order_relaxed);
    for (int32_t i = 0; i < numFrames; ++i) {
        const int n = static_cast<int>(start + i);
        const int period = s.sample_rate / 440;
        const int phase = period > 0 ? n % period : 0;
        dst[i] = phase < period / 2 ? 4000 : -4000;
    }
    const std::int64_t now = s.frames.fetch_add(numFrames, std::memory_order_relaxed) + numFrames;
    if (s.lost.load(std::memory_order_acquire) || now >= s.target_frames) {
        s.finished.store(true, std::memory_order_release);
        s.cv.notify_all();
        return AAUDIO_CALLBACK_RESULT_STOP;
    }
    return AAUDIO_CALLBACK_RESULT_CONTINUE;
}

void tone_error(AAudioStream*, void*, aaudio_result_t)
{
    Stream& s = playback_stream();
    s.lost.store(true, std::memory_order_release);
    s.finished.store(true, std::memory_order_release);
    s.cv.notify_all();
}

const char* result_name(aaudio_result_t code)
{
    switch (code) {
    case AAUDIO_OK: return "ok";
    case AAUDIO_ERROR_DISCONNECTED: return "device disconnected";
    case AAUDIO_ERROR_ILLEGAL_ARGUMENT: return "illegal argument";
    case AAUDIO_ERROR_INVALID_STATE: return "invalid state";
    case AAUDIO_ERROR_UNIMPLEMENTED: return "unimplemented";
    case AAUDIO_ERROR_UNAVAILABLE: return "unavailable";
    case AAUDIO_ERROR_NO_FREE_HANDLES: return "no free handles";
    case AAUDIO_ERROR_NO_MEMORY: return "no memory";
    case AAUDIO_ERROR_TIMEOUT: return "timeout";
    default: return "aaudio error";
    }
}

AAudioStatus fail(const std::string& reason)
{
    AAudioStatus status;
    status.available = api().ok;
    status.reason = reason;
    return status;
}

struct Opened {
    AAudioStream* stream = nullptr;
    int opened_id = -1;
    std::string error;
};

Opened open_stream(aaudio_direction_t direction, int device_id, bool specific,
                   aaudio_input_preset_t preset, bool set_preset,
                   AAudioStream_dataCallback on_data, AAudioStream_errorCallback on_error,
                   void* user, int sample_rate)
{
    Opened out;
    if (specific && device_id <= 0) {
        out.error = "aaudio id 0 is unspecified";
        return out;
    }
    Api& a = api();
    AAudioStreamBuilder* builder = nullptr;
    const aaudio_result_t created = a.create(&builder);
    if (created != AAUDIO_OK || !builder) {
        out.error = std::string("create builder failed: ") + result_name(created);
        return out;
    }
    a.setDirection(builder, direction);
    a.setSharingMode(builder, AAUDIO_SHARING_MODE_SHARED);
    a.setFormat(builder, AAUDIO_FORMAT_PCM_I16);
    a.setChannelCount(builder, 1);
    a.setSampleRate(builder, sample_rate);
    if (specific)
        a.setDeviceId(builder, device_id);
    if (set_preset && a.setInputPreset)
        a.setInputPreset(builder, preset);
    // A low-latency input dies after the first bursts on some phones. A larger
    // capacity keeps the recognition stream running for the sound trigger.
    if (direction == AAUDIO_DIRECTION_INPUT && a.setPerformanceMode)
        a.setPerformanceMode(builder, AAUDIO_PERFORMANCE_MODE_NONE);
    if (direction == AAUDIO_DIRECTION_INPUT && a.setBufferCapacity)
        a.setBufferCapacity(builder, sample_rate > 0 ? sample_rate / 2 : 8000);
    if (direction == AAUDIO_DIRECTION_OUTPUT && a.setUsage)
        a.setUsage(builder, AAUDIO_USAGE_ASSISTANT);
    if (direction == AAUDIO_DIRECTION_OUTPUT && a.setContentType)
        a.setContentType(builder, AAUDIO_CONTENT_TYPE_SPEECH);
    a.setDataCallback(builder, on_data, user);
    a.setErrorCallback(builder, on_error, user);
    AAudioStream* stream = nullptr;
    const aaudio_result_t opened = a.open(builder, &stream);
    a.destroy(builder);
    if (opened != AAUDIO_OK || !stream) {
        out.error = std::string("open failed: ") + result_name(opened);
        return out;
    }
    const int32_t got = a.getDeviceId(stream);
    if (got < 0 || (specific && got != device_id) || (!specific && got <= 0)) {
        a.close(stream);
        out.error = specific ? "opened device differs from the request"
                             : "opened device is unreadable";
        return out;
    }
    out.stream = stream;
    out.opened_id = got;
    return out;
}

void put_le(std::vector<unsigned char>& out, std::uint32_t value, int bytes)
{
    for (int i = 0; i < bytes; ++i)
        out.push_back(static_cast<unsigned char>((value >> (8 * i)) & 0xffu));
}

bool write_pcm_wav(const std::string& path, int rate, const std::vector<std::int16_t>& pcm)
{
    if (rate < 8000)
        rate = 16000;
    const auto data_bytes = static_cast<std::uint32_t>(pcm.size() * sizeof(std::int16_t));
    std::vector<unsigned char> hdr;
    hdr.reserve(44);
    hdr.push_back('R'); hdr.push_back('I'); hdr.push_back('F'); hdr.push_back('F');
    put_le(hdr, 36u + data_bytes, 4);
    hdr.push_back('W'); hdr.push_back('A'); hdr.push_back('V'); hdr.push_back('E');
    hdr.push_back('f'); hdr.push_back('m'); hdr.push_back('t'); hdr.push_back(' ');
    put_le(hdr, 16, 4);
    put_le(hdr, 1, 2);
    put_le(hdr, 1, 2);
    put_le(hdr, static_cast<std::uint32_t>(rate), 4);
    put_le(hdr, static_cast<std::uint32_t>(rate) * 2u, 4);
    put_le(hdr, 2, 2);
    put_le(hdr, 16, 2);
    hdr.push_back('d'); hdr.push_back('a'); hdr.push_back('t'); hdr.push_back('a');
    put_le(hdr, data_bytes, 4);
    std::ofstream file(path, std::ios::binary | std::ios::trunc);
    if (!file)
        return false;
    file.write(reinterpret_cast<const char*>(hdr.data()), static_cast<std::streamsize>(hdr.size()));
    if (!pcm.empty())
        file.write(reinterpret_cast<const char*>(pcm.data()), static_cast<std::streamsize>(data_bytes));
    return static_cast<bool>(file);
}

AAudioStatus finish_capture_locked(Stream& s, const std::string& wav_path)
{
    const bool lost = s.lost.load();
    const int id = s.opened_id;
    const std::int64_t frames = s.frames.load();
    const int rate = s.sample_rate > 0 ? s.sample_rate : 16000;
    close_locked(s);
    std::vector<std::int16_t> pcm;
    std::uint64_t dropped = 0;
    if (s.pcm_store) {
        dropped = s.pcm_store->dropped();
        const std::size_t n = s.pcm_store->size();
        if (n > 0) {
            pcm.resize(n);
            pcm.resize(s.pcm_store->try_read(pcm.data(), n));
        }
        s.pcm_store.reset();
    }
    s.frames.store(0);
    s.envelope.store(0, std::memory_order_relaxed);
    s.lost.store(false);
    s.opened_id = -1;
    AAudioStatus status;
    status.ok = true;
    status.available = api().ok;
    status.device_lost = lost;
    status.opened_id = id;
    status.frames = static_cast<std::int64_t>(pcm.size());
    status.sample_rate = rate;
    status.dropped = static_cast<std::int64_t>(dropped);
    status.reason = lost ? "selected device lost" : "stopped";
    if (frames > 0 && pcm.empty())
        status.reason = "recording was dropped";
    if (!wav_path.empty() && !write_pcm_wav(wav_path, rate, pcm)) {
        status.ok = false;
        status.reason = "couldn't write the recording";
    }
    return status;
}

} // namespace

bool aaudio_available()
{
    return load_api();
}

AAudioStatus aaudio_capture_start(std::uint64_t token, int device_id, bool specific, const char* preset,
                                   int sample_rate)
{
    if (!load_api())
        return fail("AAudio requires Android 8");
    if (token == 0 || token != lease_token())
        return fail("capture requires a live audio lease");
    const SessionKind holder = lease_holder();
    if (!lease_allows_capture(holder))
        return fail("this lease cannot capture");

    aaudio_input_preset_t input = AAUDIO_INPUT_PRESET_VOICE_RECOGNITION;
    if (preset && std::strcmp(preset, "communication") == 0)
        input = AAUDIO_INPUT_PRESET_VOICE_COMMUNICATION;
    else if (preset && preset[0] != '\0' && std::strcmp(preset, "recognition") != 0)
        return fail("unknown capture preset");

    Stream& s = capture_stream();
    std::lock_guard<std::mutex> lock(s.mu);
    close_locked(s);
    s.pcm_store.reset();
    s.frames.store(0);
    s.envelope.store(0, std::memory_order_relaxed);
    s.lost.store(false);
    s.error_code.store(0, std::memory_order_release);
    s.finished.store(false);

    const int requested = sample_rate >= 8000 && sample_rate <= 96000 ? sample_rate : 16000;
    Opened opened = open_stream(AAUDIO_DIRECTION_INPUT, device_id, specific, input, true,
                                capture_data, capture_error, &s, requested);
    if (!opened.stream)
        return fail(opened.error);
    int rate = requested;
    if (api().getSampleRate) {
        const int32_t got = api().getSampleRate(opened.stream);
        if (got >= 8000 && got <= 96000)
            rate = got;
    }
    s.sample_rate = rate;
    s.pcm_store = std::make_unique<pm::audio::graph::SpscRing>(static_cast<std::size_t>(rate) * 60);
    s.granted_token.store(token, std::memory_order_release);
    s.pcm.store(s.pcm_store.get(), std::memory_order_release);
    const aaudio_result_t started = api().requestStart(opened.stream);
    if (started != AAUDIO_OK) {
        s.pcm.store(nullptr, std::memory_order_release);
        s.pcm_store.reset();
        api().close(opened.stream);
        return fail(std::string("start failed: ") + result_name(started));
    }
    s.stream = opened.stream;
    s.opened_id = opened.opened_id;
    s.running.store(true);

    AAudioStatus status;
    status.ok = true;
    status.available = true;
    status.running = true;
    status.opened_id = opened.opened_id;
    status.reason = "opened device matches";
    return status;
}

AAudioStatus aaudio_capture_stop()
{
    Stream& s = capture_stream();
    std::lock_guard<std::mutex> lock(s.mu);
    return finish_capture_locked(s, "");
}

AAudioStatus aaudio_capture_stop_wav(const std::string& path)
{
    Stream& s = capture_stream();
    std::lock_guard<std::mutex> lock(s.mu);
    return finish_capture_locked(s, path);
}

AAudioStatus aaudio_capture_status()
{
    Stream& s = capture_stream();
    std::lock_guard<std::mutex> lock(s.mu);
    AAudioStatus status;
    status.ok = true;
    status.available = api().tried ? api().ok : aaudio_available();
    status.running = s.running.load() && s.stream != nullptr && !s.lost.load();
    status.device_lost = s.lost.load();
    status.opened_id = s.opened_id;
    status.frames = s.frames.load();
    status.level = s.envelope.load(std::memory_order_relaxed) / 32768.f;
    status.sample_rate = s.sample_rate;
    status.dropped = s.pcm_store ? static_cast<std::int64_t>(s.pcm_store->dropped()) : 0;
    const int error_code = s.error_code.load(std::memory_order_acquire);
    status.reason = status.device_lost
                  ? std::string("selected device lost: ") + result_name(static_cast<aaudio_result_t>(error_code))
                  : status.running ? "opened device matches"
                                   : "stopped";
    return status;
}

std::size_t aaudio_capture_read(std::int16_t* dst, std::size_t max_samples, int* rate)
{
    Stream& s = capture_stream();
    if (rate)
        *rate = s.sample_rate;
    if (!dst || max_samples == 0 || !s.running.load())
        return 0;
    auto* ring = s.pcm.load(std::memory_order_acquire);
    if (!ring)
        return 0;
    return ring->try_read(dst, max_samples);
}

AAudioStatus aaudio_play_tone(std::uint64_t token, int device_id, bool specific, int duration_ms)
{
    if (!load_api())
        return fail("AAudio requires Android 8");
    if (token == 0 || token != lease_token())
        return fail("playback requires a live audio lease");
    if (!lease_allows_playback(lease_holder()))
        return fail("this lease cannot play");
    if (duration_ms < 1)
        duration_ms = 1;
    if (duration_ms > 5000)
        duration_ms = 5000;

    Stream& s = playback_stream();
    std::unique_lock<std::mutex> lock(s.mu);
    close_locked(s);
    s.frames.store(0);
    s.lost.store(false);
    s.finished.store(false);
    s.granted_token.store(token, std::memory_order_release);
    s.sample_rate = 48000;
    s.target_frames = s.sample_rate * duration_ms / 1000;

    Opened opened = open_stream(AAUDIO_DIRECTION_OUTPUT, device_id, specific,
                                AAUDIO_INPUT_PRESET_GENERIC, false,
                                tone_data, tone_error, &s, s.sample_rate);
    if (!opened.stream)
        return fail(opened.error);
    const aaudio_result_t started = api().requestStart(opened.stream);
    if (started != AAUDIO_OK) {
        api().close(opened.stream);
        return fail(std::string("start failed: ") + result_name(started));
    }
    s.stream = opened.stream;
    s.opened_id = opened.opened_id;
    s.running.store(true);
    s.cv.wait_for(lock, std::chrono::milliseconds(duration_ms + 1000), [&] {
        return s.finished.load();
    });
    const bool lost = s.lost.load();
    const int id = s.opened_id;
    const std::int64_t frames = s.frames.load();
    close_locked(s);

    AAudioStatus status;
    status.ok = frames > 0 && !lost;
    status.available = true;
    status.device_lost = lost;
    status.opened_id = id;
    status.frames = frames;
    status.reason = lost ? "selected device lost" : status.ok ? "played" : "playback produced no frames";
    return status;
}

} // namespace pm::audio::session

#endif
