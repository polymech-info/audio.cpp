#include "pm/audio/source_host.hpp"

#include "pm/audio/constants.hpp"
#include "pm/audio/devices.hpp"
#include "pm/audio/log.hpp"
#if defined(FEATURE_AUDIO_MIX) && FEATURE_AUDIO_MIX
#include "pm/audio/mixed_capture.hpp"
#endif

#include <algorithm>
#include <atomic>
#include <cctype>
#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <cstring>
#include <memory>
#include <mutex>
#include <string>
#include <utility>
#include <vector>

namespace pm::audio {
namespace {

constexpr uint32_t k_rate     = constants::k_recording_sample_rate;
constexpr uint32_t k_channels = constants::k_recording_channels;

std::string json_string(const nlohmann::json& body, const char* key, bool& bad)
{
    if (!body.contains(key) || body[key].is_null())
        return {};
    if (!body[key].is_string()) {
        bad = true;
        return {};
    }
    return body[key].get<std::string>();
}

bool icontains(const std::string& haystack, const std::string& needle)
{
    if (needle.empty())
        return true;
    auto it = std::search(
        haystack.begin(), haystack.end(), needle.begin(), needle.end(),
        [](unsigned char a, unsigned char b) {
            return std::tolower(a) == std::tolower(b);
        });
    return it != haystack.end();
}

std::string trim_copy(std::string s)
{
    size_t b = 0;
    while (b < s.size() && std::isspace(static_cast<unsigned char>(s[b])))
        ++b;
    size_t e = s.size();
    while (e > b && std::isspace(static_cast<unsigned char>(s[e - 1])))
        --e;
    return s.substr(b, e - b);
}

bool device_named(const std::vector<DeviceInfo>& devs, const std::string& name)
{
    if (name.empty())
        return !devs.empty();
    for (const auto& d : devs)
        if (d.name == name)
            return true;
    for (const auto& d : devs)
        if (icontains(d.name, name))
            return true;
    return false;
}

nlohmann::json devices_json(const std::vector<DeviceInfo>& devs, const char* kind)
{
    nlohmann::json arr = nlohmann::json::array();
    for (const auto& d : devs) {
        if (d.name.empty())
            continue;
        arr.push_back({
            {"kind", kind},
            {"name", d.name},
            {"spec", std::string(kind) + ":" + d.name},
            {"isDefault", d.is_default},
            {"channels", d.channels},
            {"sampleRate", d.sample_rate},
        });
    }
    return arr;
}

struct Spec {
    std::string kind;
    std::string mic;
    std::string loopback;
    std::string input;
};

bool parse_spec(const std::string& raw_in,
                const std::string& mic_field,
                const std::string& loop_field,
                Spec& out,
                std::string& err)
{
    const std::string raw = trim_copy(raw_in);
    if (raw.empty() || raw.size() > 1024) {
        err = raw.empty() ? "missing input" : "invalid input";
        return false;
    }
    std::string kind;
    std::string rest;
    if (raw == "mic" || raw == "loopback" || raw == "mix") {
        kind = raw;
    } else if (raw.rfind("mic:", 0) == 0) {
        kind = "mic";
        rest = raw.substr(4);
    } else if (raw.rfind("loopback:", 0) == 0) {
        kind = "loopback";
        rest = raw.substr(9);
    } else if (raw.rfind("mix:", 0) == 0) {
        err = "mix takes input \"mix\" plus optional mic and loopback names";
        return false;
    } else {
        err = "input must be mic, loopback, or mix";
        return false;
    }
#if !defined(FEATURE_AUDIO_MIX) || !FEATURE_AUDIO_MIX
    if (kind == "mix") {
        err = "audio mix is not built into this binary";
        return false;
    }
#endif
    out.kind = kind;
    if (kind == "mic") {
        out.mic = rest;
        out.loopback.clear();
        out.input = rest.empty() ? "mic" : "mic:" + rest;
    } else if (kind == "loopback") {
        out.loopback = rest;
        out.mic.clear();
        out.input = rest.empty() ? "loopback" : "loopback:" + rest;
    } else {
        out.mic = trim_copy(mic_field);
        out.loopback = trim_copy(loop_field);
        out.input = "mix";
    }
    return true;
}

SourceReply bad_request(const std::string& err, const std::string& input = {})
{
    SourceReply reply;
    reply.status = 400;
    reply.body = {{"ok", false}, {"error", err}};
    if (!input.empty())
        reply.body["input"] = input;
    return reply;
}

bool read_fields(const nlohmann::json& body,
                 std::string& input,
                 std::string& mic,
                 std::string& loopback,
                 std::string& err)
{
    if (!body.is_object()) {
        err = "invalid JSON body";
        return false;
    }
    bool bad = false;
    input = json_string(body, "input", bad);
    mic = json_string(body, "mic", bad);
    loopback = json_string(body, "loopback", bad);
    if (bad) {
        err = "input, mic, and loopback must be strings";
        return false;
    }
    return true;
}

struct PcmRing {
    std::mutex              mu;
    std::condition_variable cv;
    std::vector<int16_t>    buf;
    size_t                  read  = 0;
    size_t                  count = 0;
    bool                    closed = false;

    PcmRing()
        : buf(static_cast<size_t>(k_rate) * k_channels)
    {
    }
};

void push_pcm(const std::shared_ptr<PcmRing>& ring, const int16_t* pcm, size_t frame_count)
{
    if (!ring || !pcm || frame_count == 0)
        return;
    size_t n = frame_count * k_channels;
    std::lock_guard<std::mutex> lk(ring->mu);
    if (ring->closed)
        return;
    const size_t cap = ring->buf.size();
    if (n >= cap) {
        pcm += n - cap;
        n = cap;
        ring->read  = 0;
        ring->count = 0;
    } else if (ring->count + n > cap) {
        const size_t drop = ring->count + n - cap;
        ring->read = (ring->read + drop) % cap;
        ring->count -= drop;
    }
    const size_t write = (ring->read + ring->count) % cap;
    const size_t first = std::min(n, cap - write);
    std::memcpy(ring->buf.data() + write, pcm, first * sizeof(int16_t));
    if (n > first)
        std::memcpy(ring->buf.data(), pcm + first, (n - first) * sizeof(int16_t));
    ring->count += n;
    ring->cv.notify_one();
}

void close_ring(const std::shared_ptr<PcmRing>& ring)
{
    if (!ring)
        return;
    std::lock_guard<std::mutex> lk(ring->mu);
    ring->closed = true;
    ring->cv.notify_all();
}

void note_pcm(std::atomic<uint64_t>& frames, std::atomic<uint32_t>& peak,
              const std::shared_ptr<PcmRing>& ring, const int16_t* pcm, size_t frame_count)
{
    if (!pcm || frame_count == 0)
        return;
    frames.fetch_add(frame_count, std::memory_order_relaxed);
    uint32_t local = 0;
    const size_t n = frame_count * k_channels;
    for (size_t i = 0; i < n; ++i) {
        int v = pcm[i];
        if (v < 0)
            v = -v;
        if (static_cast<uint32_t>(v) > local)
            local = static_cast<uint32_t>(v);
    }
    uint32_t cur = peak.load(std::memory_order_relaxed);
    while (local > cur && !peak.compare_exchange_weak(cur, local, std::memory_order_relaxed)) {
    }
    push_pcm(ring, pcm, frame_count);
}

struct Session {
    std::string input;
    std::string kind;
    std::string requested_mic;
    std::string requested_loop;
    std::string mic_name;
    std::string loop_name;
    std::atomic<uint64_t> frames{0};
    std::atomic<uint32_t> peak{0};
    std::shared_ptr<PcmRing> ring;
    std::unique_ptr<AudioInput> mic;
    std::unique_ptr<DesktopAudioCapture> loop;
#if defined(FEATURE_AUDIO_MIX) && FEATURE_AUDIO_MIX
    std::unique_ptr<MixedAudioCapture> mix;
#endif
};

bool prepare_devices(const Spec& spec, std::string& err)
{
    if (spec.kind == "mic" || spec.kind == "mix") {
        const auto mics = enumerate_capture_devices();
        if (!device_named(mics, spec.mic)) {
            err = spec.mic.empty() ? "no capture device"
                                   : "no capture device matching \"" + spec.mic + "\"";
            return false;
        }
    }
    if (spec.kind == "loopback" || spec.kind == "mix") {
        const auto loops = enumerate_loopback_devices();
        if (!device_named(loops, spec.loopback)) {
            err = spec.loopback.empty()
                      ? "no loopback device"
                      : "no loopback device matching \"" + spec.loopback + "\"";
            return false;
        }
    }
    return true;
}

std::unique_ptr<Session> open_session(const Spec& spec, std::string& err)
{
    if (!prepare_devices(spec, err))
        return nullptr;
    auto session = std::make_unique<Session>();
    session->input = spec.input;
    session->kind = spec.kind;
    session->requested_mic = spec.mic;
    session->requested_loop = spec.loopback;
    session->ring = std::make_shared<PcmRing>();
    try {
        if (spec.kind == "mic") {
            session->mic = std::make_unique<AudioInput>();
            Session* raw = session.get();
            auto ring = session->ring;
            session->mic->start(
                [raw, ring](const int16_t* pcm, size_t frames) {
                    note_pcm(raw->frames, raw->peak, ring, pcm, frames);
                },
                spec.mic, k_rate, k_channels, false);
            session->mic_name = session->mic->opened_device_name();
            return session;
        }
        if (spec.kind == "loopback") {
            session->loop = std::make_unique<DesktopAudioCapture>();
            Session* raw = session.get();
            auto ring = session->ring;
            if (!session->loop->start(
                    [raw, ring](const int16_t* pcm, size_t frames) {
                        note_pcm(raw->frames, raw->peak, ring, pcm, frames);
                    },
                    spec.loopback, err, k_rate, k_channels)) {
                if (err.empty())
                    err = "loopback capture failed";
                return nullptr;
            }
            session->loop_name = session->loop->opened_device_name();
            return session;
        }
#if defined(FEATURE_AUDIO_MIX) && FEATURE_AUDIO_MIX
        session->mix = std::make_unique<MixedAudioCapture>();
        Session* raw = session.get();
        auto ring = session->ring;
        const auto cfg = make_recording_capture_config(
            "mix", spec.mic, spec.loopback, 1.0f, 1.0f, CaptureSource::mix);
        if (!session->mix->start(
                cfg,
                [raw, ring](const int16_t* pcm, size_t frames) {
                    note_pcm(raw->frames, raw->peak, ring, pcm, frames);
                },
                err)) {
            if (err.empty())
                err = "mix capture failed";
            return nullptr;
        }
        session->mic_name = session->mix->opened_mic_device();
        session->loop_name = session->mix->opened_desktop_device();
        return session;
#else
        err = "audio mix is not built into this binary";
        return nullptr;
#endif
    } catch (const std::exception& ex) {
        err = ex.what();
        return nullptr;
    }
}

bool same_request(const Session& live, const Spec& spec)
{
    return live.kind == spec.kind && live.requested_mic == spec.mic
           && live.requested_loop == spec.loopback;
}

} // namespace

struct SourceHost::Impl {
    mutable std::mutex mu;
    std::string pending;
    std::unique_ptr<Session> live;

    nlohmann::json status_unlocked() const
    {
        const bool running = static_cast<bool>(live);
        return {
            {"ok", true},
            {"running", running},
            {"phase", running ? "capture" : "idle"},
            {"input", running ? live->input : pending},
            {"kind", running ? live->kind : std::string{}},
            {"frames", running ? live->frames.load(std::memory_order_relaxed) : 0},
            {"peak", running ? live->peak.load(std::memory_order_relaxed) : 0},
            {"sampleRate", running ? k_rate : 0},
            {"channels", running ? k_channels : 0},
            {"mic", running ? live->mic_name : std::string{}},
            {"loopback", running ? live->loop_name : std::string{}},
#if defined(FEATURE_AUDIO_MIX) && FEATURE_AUDIO_MIX
            {"mix", true},
#else
            {"mix", false},
#endif
        };
    }

    bool start_spec(const Spec& spec, std::string& err, bool& already, bool& switched)
    {
        already = false;
        switched = false;
        {
            std::lock_guard<std::mutex> lk(mu);
            if (live && same_request(*live, spec)) {
                already = true;
                return true;
            }
        }
        auto next = open_session(spec, err);
        if (!next)
            return false;
        std::unique_ptr<Session> old;
        std::shared_ptr<PcmRing> dropped;
        {
            std::lock_guard<std::mutex> lk(mu);
            if (live && same_request(*live, spec)) {
                already = true;
                old = std::move(next);
            } else {
                switched = static_cast<bool>(live);
                old = std::move(live);
                live = std::move(next);
                pending = spec.input;
            }
            if (old)
                dropped = old->ring;
        }
        close_ring(dropped);
        old.reset();
        return true;
    }

    std::unique_ptr<Session> release()
    {
        std::shared_ptr<PcmRing> ring;
        std::unique_ptr<Session> old;
        {
            std::lock_guard<std::mutex> lk(mu);
            if (live)
                ring = live->ring;
            old = std::move(live);
        }
        close_ring(ring);
        return old;
    }

    int pull(int16_t* dst, int max_frames, int wait_ms)
    {
        std::shared_ptr<PcmRing> ring;
        {
            std::lock_guard<std::mutex> lk(mu);
            if (!live || max_frames <= 0 || !dst)
                return -1;
            ring = live->ring;
        }
        if (!ring)
            return -1;
        const size_t want = static_cast<size_t>(max_frames) * k_channels;
        std::unique_lock<std::mutex> lk(ring->mu);
        if (ring->count == 0 && !ring->closed) {
            ring->cv.wait_for(lk, std::chrono::milliseconds(wait_ms), [&] {
                return ring->count > 0 || ring->closed;
            });
        }
        if (ring->count == 0)
            return ring->closed ? -1 : 0;
        size_t n = std::min(want, ring->count);
        n -= n % k_channels;
        if (n == 0)
            return 0;
        const size_t cap = ring->buf.size();
        const size_t first = std::min(n, cap - ring->read);
        std::memcpy(dst, ring->buf.data() + ring->read, first * sizeof(int16_t));
        if (n > first)
            std::memcpy(dst + first, ring->buf.data(), (n - first) * sizeof(int16_t));
        ring->read = (ring->read + n) % cap;
        ring->count -= n;
        return static_cast<int>(n / k_channels);
    }
};

SourceHost::SourceHost()
    : impl_(std::make_shared<Impl>())
{
}

SourceHost::~SourceHost()
{
    stop();
}

void SourceHost::stop()
{
    auto old = impl_->release();
    old.reset();
}

int SourceHost::pull(std::int16_t* dst, int max_frames, int wait_ms)
{
    return impl_->pull(dst, max_frames, wait_ms);
}

SourceReply SourceHost::status() const
{
    std::lock_guard<std::mutex> lk(impl_->mu);
    return {200, impl_->status_unlocked()};
}

SourceReply SourceHost::sources()
{
    nlohmann::json out = {
        {"ok", true},
        {"mics", devices_json(enumerate_capture_devices(), "mic")},
        {"loopback", devices_json(enumerate_loopback_devices(), "loopback")},
#if defined(FEATURE_AUDIO_MIX) && FEATURE_AUDIO_MIX
        {"mix", true},
#else
        {"mix", false},
#endif
    };
    std::lock_guard<std::mutex> lk(impl_->mu);
    out["current"] = impl_->live ? impl_->live->input : impl_->pending;
    return {200, std::move(out)};
}

SourceReply SourceHost::start(const nlohmann::json& body)
{
    std::string input;
    std::string mic;
    std::string loopback;
    std::string err;
    if (!read_fields(body, input, mic, loopback, err))
        return bad_request(err);
    if (trim_copy(input).empty()) {
        std::lock_guard<std::mutex> lk(impl_->mu);
        input = impl_->pending;
    }
    Spec spec;
    if (!parse_spec(input, mic, loopback, spec, err))
        return bad_request(err);
    bool already = false;
    bool switched = false;
    if (!impl_->start_spec(spec, err, already, switched)) {
        log::trace("[audio-source] start failed input=" + spec.input + " err=" + err);
        auto reply = bad_request(err, spec.input);
        std::lock_guard<std::mutex> lk(impl_->mu);
        if (impl_->live)
            reply.body["phase"] = "capture";
        return reply;
    }
    auto reply = status();
    reply.body["accepted"] = true;
    reply.body["already"] = already;
    reply.body["switched"] = switched;
    return reply;
}

SourceReply SourceHost::select(const nlohmann::json& body)
{
    std::string input;
    std::string mic;
    std::string loopback;
    std::string err;
    if (!read_fields(body, input, mic, loopback, err))
        return bad_request(err);
    Spec spec;
    if (!parse_spec(input, mic, loopback, spec, err))
        return bad_request(err);
    bool running = false;
    {
        std::lock_guard<std::mutex> lk(impl_->mu);
        running = static_cast<bool>(impl_->live);
        if (!running)
            impl_->pending = spec.input;
    }
    bool already = false;
    bool switched = false;
    if (running && !impl_->start_spec(spec, err, already, switched)) {
        log::trace("[audio-source] switch failed input=" + spec.input + " err=" + err);
        return bad_request(err, spec.input);
    }
    auto reply = status();
    reply.body["accepted"] = true;
    reply.body["switched"] = switched;
    reply.body["stored"] = !running;
    return reply;
}

} // namespace pm::audio
