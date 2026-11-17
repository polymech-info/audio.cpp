#include "pm/audio/sink_host.hpp"

#include "pm/audio/constants.hpp"
#include "pm/audio/devices.hpp"
#include "pm/audio/log.hpp"
#include "pm/audio/ring.hpp"

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
#include <cmath>
#include <cstdint>
#include <cstring>
#include <mutex>
#include <string>
#include <vector>

namespace pm::audio {
namespace {

constexpr uint32_t k_rate         = constants::k_recording_sample_rate;
constexpr uint32_t k_channels     = constants::k_recording_channels;
constexpr int      k_prebuffer_ms = 200;
constexpr int64_t  k_report_ns    = 2'000'000'000LL;

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

std::string wanted_name(const nlohmann::json& body, bool& bad)
{
    std::string name = json_string(body, "device", bad);
    if (name.empty())
        name = json_string(body, "name", bad);
    if (name.empty())
        name = json_string(body, "output", bad);
    return name;
}

bool icontains(const std::string& haystack, const std::string& needle)
{
    if (needle.empty())
        return false;
    auto it = std::search(
        haystack.begin(), haystack.end(), needle.begin(), needle.end(),
        [](unsigned char a, unsigned char b) {
            return std::tolower(a) == std::tolower(b);
        });
    return it != haystack.end();
}

bool same_name(const std::string& a, const std::string& b)
{
    if (a.size() != b.size())
        return false;
    for (size_t i = 0; i < a.size(); ++i) {
        if (std::tolower(static_cast<unsigned char>(a[i])) !=
            std::tolower(static_cast<unsigned char>(b[i])))
            return false;
    }
    return true;
}

bool resolve_output(const std::vector<DeviceInfo>& devs,
                    const std::string& want,
                    std::string& chosen,
                    std::string& err)
{
    if (want.size() > 1024) {
        err = "device name is too long";
        return false;
    }
    if (devs.empty()) {
        err = "no playback device";
        return false;
    }
    if (want.empty()) {
        for (const auto& dev : devs) {
            if (dev.is_default && !dev.name.empty()) {
                chosen = dev.name;
                return true;
            }
        }
        chosen = devs.front().name;
        return !chosen.empty();
    }
    for (const auto& dev : devs) {
        if (same_name(dev.name, want)) {
            chosen = dev.name;
            return true;
        }
    }
    int hits = 0;
    std::string partial;
    for (const auto& dev : devs) {
        if (!icontains(dev.name, want))
            continue;
        partial = dev.name;
        ++hits;
    }
    if (hits == 1) {
        chosen = partial;
        return true;
    }
    err = hits > 1 ? "playback device name is ambiguous" : "no playback device named " + want;
    return false;
}

struct ResampleCarry {
    uint32_t    rate     = 0;
    uint32_t    channels = 0;
    double      pos      = 0;
    std::int16_t hold[2] = {};
    bool        held     = false;
};

int64_t steady_ns()
{
    return std::chrono::duration_cast<std::chrono::nanoseconds>(
               std::chrono::steady_clock::now().time_since_epoch())
        .count();
}

int queued_ms(size_t bytes)
{
    const size_t per_ms = static_cast<size_t>(k_rate) * k_channels * sizeof(std::int16_t) / 1000;
    if (per_ms == 0)
        return 0;
    return static_cast<int>(bytes / per_ms);
}

void to_wire(ResampleCarry& carry,
             const std::int16_t* pcm,
             int frames,
             uint32_t rate,
             uint32_t channels,
             std::vector<std::int16_t>& out)
{
    out.clear();
    if (!pcm || frames <= 0)
        return;
    if (rate < 8000)
        rate = 8000;
    if (rate > 96000)
        rate = 96000;
    if (channels < 1)
        channels = 1;
    if (channels > 8)
        channels = 8;

    if (rate == k_rate && (channels == k_channels || channels == 1)) {
        carry.rate = rate;
        carry.channels = channels;
        carry.pos = 0;
        carry.held = false;
        if (channels == k_channels) {
            out.assign(pcm, pcm + static_cast<size_t>(frames) * channels);
            return;
        }
        out.resize(static_cast<size_t>(frames) * k_channels);
        for (int i = 0; i < frames; ++i) {
            out[static_cast<size_t>(i) * 2]     = pcm[i];
            out[static_cast<size_t>(i) * 2 + 1] = pcm[i];
        }
        return;
    }

    if (carry.rate != rate || carry.channels != channels) {
        carry.rate = rate;
        carry.channels = channels;
        carry.pos = 0;
        carry.held = false;
    }

    const double step = static_cast<double>(rate) / static_cast<double>(k_rate);
    double pos = carry.pos;
    auto at = [&](int index, int ch) -> int {
        if (index < 0)
            return carry.hold[ch];
        const int src = channels == 1 ? 0 : std::min(ch, static_cast<int>(channels) - 1);
        if (index >= frames)
            index = frames - 1;
        return pcm[static_cast<size_t>(index) * channels + static_cast<size_t>(src)];
    };
    // Stop before an output sample would need the next chunk. The leftover
    // position, including a negative one, is the start of the following push.
    while (pos + 1.0 < static_cast<double>(frames)) {
        const int i0 = static_cast<int>(std::floor(pos));
        const double frac = pos - static_cast<double>(i0);
        for (int ch = 0; ch < static_cast<int>(k_channels); ++ch) {
            const int s0 = at(i0, ch);
            const int s1 = at(i0 + 1, ch);
            const int mixed = static_cast<int>(std::lround(s0 + (s1 - s0) * frac));
            out.push_back(static_cast<std::int16_t>(std::clamp(mixed, -32768, 32767)));
        }
        pos += step;
    }
    const int last = frames - 1;
    for (int ch = 0; ch < static_cast<int>(k_channels); ++ch)
        carry.hold[ch] = static_cast<std::int16_t>(at(last, ch));
    carry.held = true;
    carry.pos = pos - static_cast<double>(frames);
}

} // namespace

struct SinkHost::Impl {
    std::mutex control;
    std::string device;
    AudioRingBuffer ring;
    std::vector<std::int16_t> scratch;
    ResampleCarry carry;
    std::atomic<std::uint64_t> dropped{0};
    std::atomic<std::uint64_t> underrun_frames{0};
    std::atomic<std::uint64_t> underrun_events{0};
    std::atomic<std::uint64_t> chunks{0};
    std::atomic<bool> running{false};
    ma_device playback{};
    bool device_open = false;
    bool primed = false;
    uint32_t in_rate = 0;
    uint32_t in_channels = 0;
    int64_t last_push_ns = 0;
    int64_t last_report_ns = 0;
    int max_gap_ms = 0;
    std::uint64_t reported_underruns = 0;
    std::uint64_t reported_dropped = 0;

    void reset_ring()
    {
        ring.cancelled.store(false, std::memory_order_relaxed);
        ring.done.store(false, std::memory_order_relaxed);
        ring.wpos.store(0, std::memory_order_relaxed);
        ring.rpos.store(0, std::memory_order_relaxed);
    }

    void close_device()
    {
        if (!device_open)
            return;
        ma_device_stop(&playback);
        ma_device_uninit(&playback);
        device_open = false;
        running.store(false, std::memory_order_release);
    }

    static void data_callback(ma_device* device, void* output, const void* /*input*/, ma_uint32 frame_count)
    {
        auto* self = static_cast<Impl*>(device->pUserData);
        auto* dst = static_cast<std::int16_t*>(output);
        const size_t bytes = static_cast<size_t>(frame_count) * k_channels * sizeof(std::int16_t);
        const size_t got = self->ring.pop(dst, bytes);
        if (got < bytes) {
            std::memset(reinterpret_cast<char*>(dst) + got, 0, bytes - got);
            const size_t missing = (bytes - got) / (k_channels * sizeof(std::int16_t));
            self->underrun_frames.fetch_add(missing, std::memory_order_relaxed);
            self->underrun_events.fetch_add(1, std::memory_order_relaxed);
        }
    }

    bool open_device(const std::string& name, std::string& err)
    {
        close_device();
        reset_ring();
        carry = {};
        primed = false;
        in_rate = 0;
        in_channels = 0;
        last_push_ns = 0;
        last_report_ns = 0;
        max_gap_ms = 0;
        reported_underruns = 0;
        reported_dropped = 0;
        dropped.store(0, std::memory_order_relaxed);
        underrun_frames.store(0, std::memory_order_relaxed);
        underrun_events.store(0, std::memory_order_relaxed);
        chunks.store(0, std::memory_order_relaxed);

        ma_device_config cfg = ma_device_config_init(ma_device_type_playback);
        cfg.playback.format   = ma_format_s16;
        cfg.playback.channels = k_channels;
        cfg.sampleRate        = k_rate;
        cfg.dataCallback      = data_callback;
        cfg.pUserData         = this;

        ma_device_id named{};
        bool found = false;
        if (!name.empty()) {
            ma_context enum_ctx{};
            if (ma_context_init(nullptr, 0, nullptr, &enum_ctx) == MA_SUCCESS) {
                ma_device_info* outs = nullptr;
                ma_uint32 nouts = 0;
                ma_device_info* ins = nullptr;
                ma_uint32 nins = 0;
                if (ma_context_get_devices(&enum_ctx, &outs, &nouts, &ins, &nins) == MA_SUCCESS) {
                    for (ma_uint32 i = 0; i < nouts; ++i) {
                        if (same_name(outs[i].name, name)) {
                            named = outs[i].id;
                            cfg.playback.pDeviceID = &named;
                            found = true;
                            break;
                        }
                    }
                }
                ma_context_uninit(&enum_ctx);
            }
            if (!found) {
                err = "playback device disappeared";
                return false;
            }
        }

        if (ma_device_init(nullptr, &cfg, &playback) != MA_SUCCESS) {
            err = "playback device did not open";
            return false;
        }
        // Stay quiet until a cushion is queued. Starting empty plays a gap
        // between every chunk that arrives over the network.
        device = name;
        device_open = true;
        running.store(true, std::memory_order_release);
        return true;
    }

    bool prime()
    {
        if (primed || !device_open)
            return primed;
        const size_t need = static_cast<size_t>(k_rate) * k_channels * sizeof(std::int16_t) *
                            static_cast<size_t>(k_prebuffer_ms) / 1000;
        if (ring.readable() < need)
            return false;
        if (ma_device_start(&playback) != MA_SUCCESS) {
            pm::audio::log::warn("[audio] playback device did not start");
            return false;
        }
        primed = true;
        pm::audio::log::info("[audio] playback audible \"" + device + "\" cushion_ms=" +
                             std::to_string(k_prebuffer_ms) + " in=" + std::to_string(in_rate) +
                             "x" + std::to_string(in_channels));
        return true;
    }

    void report(bool force)
    {
        const int64_t now = steady_ns();
        const auto underrun = underrun_frames.load(std::memory_order_relaxed);
        const auto dropped_now = dropped.load(std::memory_order_relaxed);
        const bool worse = underrun != reported_underruns || dropped_now != reported_dropped;
        if (!force && !worse)
            return;
        if (!force && last_report_ns != 0 && now - last_report_ns < k_report_ns)
            return;
        const auto underrun_ms = underrun * 1000 / k_rate;
        const auto added = underrun - reported_underruns;
        const int queued = queued_ms(ring.readable());
        last_report_ns = now;
        reported_underruns = underrun;
        reported_dropped = dropped_now;
        const int gap = max_gap_ms;
        max_gap_ms = 0;
        std::string line = "[audio] playback " + std::string(force ? "stopped" : "gap") +
                           " underrun_ms=" + std::to_string(underrun_ms) +
                           " added_frames=" + std::to_string(added) +
                           " events=" + std::to_string(underrun_events.load(std::memory_order_relaxed)) +
                           " queued_ms=" + std::to_string(queued) +
                           " dropped=" + std::to_string(dropped_now) +
                           " gap_ms=" + std::to_string(gap) +
                           " chunks=" + std::to_string(chunks.load(std::memory_order_relaxed)) +
                           " in=" + std::to_string(in_rate) + "x" + std::to_string(in_channels);
        if (force && !worse)
            pm::audio::log::info(line);
        else
            pm::audio::log::warn(line);
    }

    nlohmann::json status_json() const
    {
        return {
            {"ok", true},
            {"running", running.load(std::memory_order_acquire)},
            {"device", device},
            {"sampleRate", k_rate},
            {"channels", k_channels},
            {"queued", ring.readable()},
            {"queuedMs", queued_ms(ring.readable())},
            {"primed", primed},
            {"dropped", dropped.load(std::memory_order_relaxed)},
            {"underrunFrames", underrun_frames.load(std::memory_order_relaxed)},
            {"inputRate", in_rate},
            {"inputChannels", in_channels},
        };
    }
};

SinkHost::SinkHost()
    : impl_(std::make_shared<Impl>())
{
}

SinkHost::~SinkHost()
{
    stop();
}

void SinkHost::stop()
{
    std::lock_guard<std::mutex> lock(impl_->control);
    if (impl_->device_open)
        impl_->report(true);
    impl_->close_device();
    impl_->reset_ring();
}

SinkReply SinkHost::status() const
{
    std::lock_guard<std::mutex> lock(impl_->control);
    return {200, impl_->status_json()};
}

SinkReply SinkHost::outputs()
{
    nlohmann::json rows = nlohmann::json::array();
    for (const auto& dev : enumerate_playback_devices()) {
        if (dev.name.empty())
            continue;
        rows.push_back({
            {"name", dev.name},
            {"spec", std::string("output:") + dev.name},
            {"isDefault", dev.is_default},
            {"channels", dev.channels},
            {"sampleRate", dev.sample_rate},
        });
    }
    std::lock_guard<std::mutex> lock(impl_->control);
    nlohmann::json body = impl_->status_json();
    body["outputs"] = std::move(rows);
    return {200, std::move(body)};
}

SinkReply SinkHost::select(const nlohmann::json& body)
{
    bool bad = false;
    const std::string want = wanted_name(body, bad);
    if (bad)
        return {400, {{"ok", false}, {"error", "device must be a string"}}};
    std::string chosen;
    std::string err;
    if (!resolve_output(enumerate_playback_devices(), want, chosen, err))
        return {404, {{"ok", false}, {"error", err}}};

    std::lock_guard<std::mutex> lock(impl_->control);
    if (!impl_->running.load(std::memory_order_acquire)) {
        impl_->device = chosen;
        auto idle = impl_->status_json();
        idle["selected"] = true;
        return {200, std::move(idle)};
    }
    if (impl_->device == chosen) {
        auto same = impl_->status_json();
        same["selected"] = true;
        return {200, std::move(same)};
    }
    if (!impl_->open_device(chosen, err))
        return {500, {{"ok", false}, {"error", err}, {"device", chosen}}};
    auto switched = impl_->status_json();
    switched["selected"] = true;
    pm::audio::log::info("[audio] playback output \"" + chosen + "\"");
    return {200, std::move(switched)};
}

SinkReply SinkHost::start(const nlohmann::json& body)
{
    bool bad = false;
    const std::string want = wanted_name(body, bad);
    if (bad)
        return {400, {{"ok", false}, {"error", "device must be a string"}}};

    std::string chosen;
    std::string err;
    {
        std::lock_guard<std::mutex> lock(impl_->control);
        const std::string name = want.empty() ? impl_->device : want;
        if (!resolve_output(enumerate_playback_devices(), name, chosen, err))
            return {404, {{"ok", false}, {"error", err}}};
        if (impl_->running.load(std::memory_order_acquire) && impl_->device == chosen) {
            auto same = impl_->status_json();
            same["started"] = true;
            return {200, std::move(same)};
        }
        if (!impl_->open_device(chosen, err))
            return {500, {{"ok", false}, {"error", err}, {"device", chosen}}};
    }
    pm::audio::log::info("[audio] playback armed \"" + chosen + "\"");
    std::lock_guard<std::mutex> lock(impl_->control);
    auto started = impl_->status_json();
    started["started"] = true;
    return {200, std::move(started)};
}

SinkReply SinkHost::push(const std::int16_t* pcm, int frames, std::uint32_t rate, std::uint32_t channels)
{
    std::lock_guard<std::mutex> lock(impl_->control);
    if (!impl_->running.load(std::memory_order_acquire) || !pcm || frames <= 0)
        return {409, {{"ok", false}, {"error", "playback is idle"}}};

    const uint32_t in_rate = rate == 0 ? k_rate : rate;
    const uint32_t in_channels = channels == 0 ? 1 : channels;
    const int64_t now = steady_ns();
    if (impl_->last_push_ns != 0) {
        const int gap = static_cast<int>((now - impl_->last_push_ns) / 1'000'000LL);
        if (gap > impl_->max_gap_ms)
            impl_->max_gap_ms = gap;
    }
    impl_->last_push_ns = now;
    impl_->in_rate = in_rate;
    impl_->in_channels = in_channels;
    const auto seen = impl_->chunks.fetch_add(1, std::memory_order_relaxed);
    if (seen == 0) {
        pm::audio::log::info("[audio] playback input " + std::to_string(in_rate) + " Hz x" +
                             std::to_string(in_channels) + " frames=" + std::to_string(frames));
    }
    to_wire(impl_->carry, pcm, frames, in_rate, in_channels, impl_->scratch);
    const size_t want = impl_->scratch.size() * sizeof(std::int16_t);
    const size_t space = AudioRingBuffer::k_cap - impl_->ring.readable();
    const size_t room = space - (space % (k_channels * sizeof(std::int16_t)));
    const size_t give = std::min(want, room);
    const size_t took = give == 0 ? 0 : impl_->ring.try_push(impl_->scratch.data(), give);
    if (took < want)
        impl_->dropped.fetch_add((want - took) / (k_channels * sizeof(std::int16_t)), std::memory_order_relaxed);
    impl_->prime();
    impl_->report(false);
    auto body = impl_->status_json();
    body["accepted"] = took / (k_channels * sizeof(std::int16_t));
    return {200, std::move(body)};
}

} // namespace pm::audio
