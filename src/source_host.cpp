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
#include <cmath>
#include <condition_variable>
#include <cstdint>
#include <cstring>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <utility>
#include <vector>

namespace pm::audio {
namespace {

constexpr uint32_t k_rate     = constants::k_recording_sample_rate;
constexpr uint32_t k_channels = constants::k_recording_channels;

std::atomic<uint32_t> g_default_rate{k_rate};
std::atomic<uint32_t> g_default_bits{16};

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

bool device_named(const std::vector<DeviceInfo>& devs, const std::string& name, std::string& err)
{
    if (name.size() > 1024) {
        err = "device name is too long";
        return false;
    }
    if (name.empty()) {
        if (devs.empty()) {
            err = "no device";
            return false;
        }
        return true;
    }
    int exact = 0;
    int partial = 0;
    for (const auto& d : devs) {
        if (d.name == name)
            ++exact;
        else if (icontains(d.name, name))
            ++partial;
    }
    if (exact == 1 || (exact == 0 && partial == 1))
        return true;
    if (exact > 1 || partial > 1) {
        err = "more than one device matches \"" + name + "\"";
        return false;
    }
    err = "no device matching \"" + name + "\"";
    return false;
}

int64_t steady_ns()
{
    return std::chrono::duration_cast<std::chrono::nanoseconds>(
               std::chrono::steady_clock::now().time_since_epoch())
        .count();
}

uint32_t fresh_peak(uint32_t peak, int64_t at, int64_t now)
{
    if (at == 0 || now - at > 280'000'000)
        return 0;
    return peak;
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

struct LegSpec {
    std::string spec;
    std::string kind;
    std::string name;
    float       gain = 1.f;
};

struct Spec {
    std::string kind;
    std::string mic;
    std::string loopback;
    std::string input;
    std::uint32_t sample_rate = k_rate;
    std::uint32_t bits = 16;
    std::vector<LegSpec> legs;
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

bool split_leg(const std::string& raw_in, LegSpec& out, std::string& err)
{
    const std::string raw = trim_copy(raw_in);
    if (raw.rfind("mic:", 0) == 0) {
        out.kind = "mic";
        out.name = trim_copy(raw.substr(4));
    } else if (raw.rfind("loopback:", 0) == 0) {
        out.kind = "loopback";
        out.name = trim_copy(raw.substr(9));
    } else {
        err = "a source spec is mic:<name> or loopback:<name>";
        return false;
    }
    if (out.name.empty() || out.name.size() > 1024) {
        err = "a source needs a device name";
        return false;
    }
    out.spec = out.kind + ":" + out.name;
    return true;
}

/// sources[] present means the client owns the mix. Absent means the legacy input.
bool parse_leg_list(const nlohmann::json& body, std::vector<LegSpec>& legs, std::string& err)
{
    legs.clear();
    if (!body.is_object() || !body.contains("sources") || body["sources"].is_null())
        return true;
    if (!body["sources"].is_array()) {
        err = "sources must be a list";
        return false;
    }
    const auto& arr = body["sources"];
    if (arr.empty()) {
        err = "sources is empty";
        return false;
    }
    if (arr.size() > 8) {
        err = "too many sources";
        return false;
    }
#if !defined(FEATURE_AUDIO_MIX) || !FEATURE_AUDIO_MIX
    err = "audio mix is not built into this binary";
    return false;
#else
    for (const auto& item : arr) {
        if (!item.is_object()) {
            err = "each source needs a spec";
            return false;
        }
        bool bad = false;
        const std::string spec = json_string(item, "spec", bad);
        if (bad || spec.empty()) {
            err = "each source needs a spec";
            return false;
        }
        float gain = 1.f;
        if (item.contains("gain") && !item["gain"].is_null()) {
            if (!item["gain"].is_number()) {
                err = "gain must be a number";
                return false;
            }
            const double raw = item["gain"].get<double>();
            if (!std::isfinite(raw)) {
                err = "gain must be a finite number";
                return false;
            }
            gain = std::clamp(static_cast<float>(raw), 0.f, 2.f);
        }
        LegSpec leg;
        if (!split_leg(spec, leg, err))
            return false;
        leg.gain = gain;
        for (const auto& prev : legs) {
            if (prev.spec == leg.spec) {
                err = "duplicate source";
                return false;
            }
        }
        legs.push_back(std::move(leg));
    }
    return true;
#endif
}

void fill_bus(Spec& spec)
{
    spec.mic.clear();
    spec.loopback.clear();
    if (spec.legs.size() == 1) {
        spec.kind  = spec.legs[0].kind;
        spec.input = spec.legs[0].spec;
    } else {
        spec.kind  = "mix";
        spec.input = "mix";
    }
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

bool read_format(const nlohmann::json& body, Spec& spec, bool keep, std::string& err)
{
    std::uint32_t rate = keep ? spec.sample_rate : g_default_rate.load();
    std::uint32_t bits = keep ? spec.bits : g_default_bits.load();
    auto take = [&](const char* key, std::uint32_t& dst) -> bool {
        if (!body.contains(key) || body[key].is_null())
            return true;
        if (!body[key].is_number_integer() && !body[key].is_number_unsigned()) {
            err = std::string(key) + " must be a number";
            return false;
        }
        const auto n = body[key].get<std::int64_t>();
        if (n <= 0 || n > 384000) {
            err = std::string(key) + " is out of range";
            return false;
        }
        dst = static_cast<std::uint32_t>(n);
        return true;
    };
    if (!take("sampleRate", rate) || !take("bits", bits))
        return false;
    if (rate != 16000 && rate != 24000 && rate != constants::k_recording_sample_rate) {
        err = "sampleRate must be 16000, 24000, or 48000";
        return false;
    }
    if (bits != 16) {
        err = "bits must be 16";
        return false;
    }
    spec.sample_rate = rate;
    spec.bits = bits;
    return true;
}

struct Cursor {
    uint64_t id = 0;
    uint64_t at = 0;
};

struct PcmRing {
    std::mutex              mu;
    std::condition_variable cv;
    std::vector<int16_t>    buf;
    size_t                  write = 0;
    uint64_t                produced = 0;
    bool                    closed = false;
    std::atomic<uint64_t>   dropped{0};
    std::vector<Cursor>     cursors;
    uint64_t                next_cursor = 1;
    uint64_t                legacy = 0;
    uint32_t                rate = k_rate;

    explicit PcmRing(uint32_t sample_rate)
        : buf(static_cast<size_t>(sample_rate) * k_channels)
        , rate(sample_rate)
    {
    }
};

void push_pcm(const std::shared_ptr<PcmRing>& ring, const int16_t* pcm, size_t frame_count)
{
    if (!ring || !pcm || frame_count == 0)
        return;
    size_t n = frame_count * k_channels;
    bool wake = false;
    {
        std::lock_guard<std::mutex> lk(ring->mu);
        if (ring->closed)
            return;
        const size_t cap = ring->buf.size();
        const size_t incoming = n;
        if (n >= cap) {
            pcm += n - cap;
            std::memcpy(ring->buf.data(), pcm, cap * sizeof(int16_t));
            ring->write = 0;
        } else {
            const size_t first = std::min(n, cap - ring->write);
            std::memcpy(ring->buf.data() + ring->write, pcm, first * sizeof(int16_t));
            if (n > first)
                std::memcpy(ring->buf.data(), pcm + first, (n - first) * sizeof(int16_t));
            ring->write = (ring->write + n) % cap;
        }
        ring->produced += incoming;
        for (auto& cursor : ring->cursors) {
            if (ring->produced > cursor.at + cap) {
                const uint64_t jump = ring->produced - cap - cursor.at;
                cursor.at = ring->produced - cap;
                ring->dropped.fetch_add(jump / k_channels, std::memory_order_relaxed);
            }
        }
        wake = true;
    }
    if (wake)
        ring->cv.notify_all();
}

uint64_t add_cursor(const std::shared_ptr<PcmRing>& ring)
{
    std::lock_guard<std::mutex> lk(ring->mu);
    Cursor cursor;
    cursor.id = ring->next_cursor++;
    cursor.at = ring->produced;
    ring->cursors.push_back(cursor);
    return cursor.id;
}

void remove_cursor(const std::shared_ptr<PcmRing>& ring, uint64_t id)
{
    if (!ring || id == 0)
        return;
    std::lock_guard<std::mutex> lk(ring->mu);
    for (auto it = ring->cursors.begin(); it != ring->cursors.end(); ++it) {
        if (it->id != id)
            continue;
        ring->cursors.erase(it);
        if (ring->legacy == id)
            ring->legacy = 0;
        return;
    }
}

int read_cursor(const std::shared_ptr<PcmRing>& ring, uint64_t id, int16_t* dst, int max_frames, int wait_ms)
{
    if (!ring || !dst || max_frames <= 0 || id == 0)
        return -1;
    const size_t want = static_cast<size_t>(max_frames) * k_channels;
    if (wait_ms < 0)
        wait_ms = 0;
    if (wait_ms > 5000)
        wait_ms = 5000;
    std::unique_lock<std::mutex> lk(ring->mu);
    auto find = [&]() -> Cursor* {
        for (auto& item : ring->cursors) {
            if (item.id == id)
                return &item;
        }
        return nullptr;
    };
    Cursor* cursor = find();
    if (!cursor)
        return -1;
    const uint64_t seen = cursor->at;
    if (seen == ring->produced && !ring->closed) {
        ring->cv.wait_for(lk, std::chrono::milliseconds(wait_ms), [&] {
            return ring->produced > seen || ring->closed;
        });
        cursor = find();
        if (!cursor)
            return -1;
    }
    if (cursor->at >= ring->produced)
        return ring->closed ? -1 : 0;
    const size_t cap = ring->buf.size();
    if (ring->produced > cursor->at + cap) {
        const uint64_t jump = ring->produced - cap - cursor->at;
        cursor->at = ring->produced - cap;
        ring->dropped.fetch_add(jump / k_channels, std::memory_order_relaxed);
    }
    size_t n = static_cast<size_t>(std::min<uint64_t>(want, ring->produced - cursor->at));
    n -= n % k_channels;
    if (n == 0)
        return 0;
    const size_t age = static_cast<size_t>(ring->produced - cursor->at);
    const size_t pos = (ring->write + cap - age) % cap;
    const size_t first = std::min(n, cap - pos);
    std::memcpy(dst, ring->buf.data() + pos, first * sizeof(int16_t));
    if (n > first)
        std::memcpy(dst + first, ring->buf.data(), (n - first) * sizeof(int16_t));
    cursor->at += n;
    return static_cast<int>(n / k_channels);
}

void close_ring(const std::shared_ptr<PcmRing>& ring)
{
    if (!ring)
        return;
    {
        std::lock_guard<std::mutex> lk(ring->mu);
        ring->closed = true;
    }
    ring->cv.notify_all();
}

struct Levels {
    std::atomic<uint64_t> frames{0};
    std::atomic<uint32_t> peak{0};
    std::atomic<int64_t>  peak_at{0};
    std::atomic<int64_t>  last_ns{0};
};

void note_pcm(const std::shared_ptr<Levels>& levels,
              const std::shared_ptr<PcmRing>& ring, const int16_t* pcm, size_t frame_count)
{
    if (!levels || !pcm || frame_count == 0)
        return;
    levels->frames.fetch_add(frame_count, std::memory_order_relaxed);
    uint32_t local = 0;
    const size_t n = frame_count * k_channels;
    for (size_t i = 0; i < n; ++i) {
        int v = pcm[i];
        if (v < 0)
            v = -v;
        if (static_cast<uint32_t>(v) > local)
            local = static_cast<uint32_t>(v);
    }
    const int64_t now = steady_ns();
    levels->last_ns.store(now, std::memory_order_relaxed);
    const uint32_t cur = levels->peak.load(std::memory_order_relaxed);
    const int64_t at   = levels->peak_at.load(std::memory_order_relaxed);
    if (local >= cur || now - at > 280'000'000) {
        levels->peak.store(local, std::memory_order_relaxed);
        levels->peak_at.store(now, std::memory_order_relaxed);
    }
    push_pcm(ring, pcm, frame_count);
}

struct Leg {
    std::string spec;
    std::string kind;
    std::string requested;
    std::string opened;
    std::atomic<float> gain{1.f};
    std::atomic<uint32_t> peak{0};
    std::atomic<int64_t> peak_at{0};
    std::atomic<int64_t> last_ns{0};
    std::atomic<uint64_t> dropped{0};
    std::atomic<uint64_t> underruns{0};
    std::mutex mu;
    std::vector<int16_t> buf;
    size_t read  = 0;
    size_t count = 0;
    std::unique_ptr<AudioInput> mic;
    std::unique_ptr<DesktopAudioCapture> loop;

    Leg()
        : buf(static_cast<size_t>(k_rate) * k_channels)
    {
    }

    void push(const int16_t* pcm, size_t frames)
    {
        if (!pcm || frames == 0)
            return;
        const size_t n = frames * k_channels;
        uint32_t local = 0;
        for (size_t i = 0; i < n; ++i) {
            int v = pcm[i];
            if (v < 0)
                v = -v;
            if (static_cast<uint32_t>(v) > local)
                local = static_cast<uint32_t>(v);
        }
        const int64_t now = steady_ns();
        last_ns.store(now, std::memory_order_relaxed);
        const uint32_t cur = peak.load(std::memory_order_relaxed);
        const int64_t at   = peak_at.load(std::memory_order_relaxed);
        if (local >= cur || now - at > 280'000'000) {
            peak.store(local, std::memory_order_relaxed);
            peak_at.store(now, std::memory_order_relaxed);
        }
        std::lock_guard<std::mutex> lk(mu);
        const size_t cap = buf.size();
        size_t samples = n;
        const int16_t* src = pcm;
        size_t overflow = 0;
        if (samples >= cap) {
            overflow = count + (samples - cap);
            src += samples - cap;
            samples = cap;
            read = 0;
            count = 0;
        } else if (count + samples > cap) {
            overflow = count + samples - cap;
            read = (read + overflow) % cap;
            count -= overflow;
        }
        if (overflow > 0)
            dropped.fetch_add(overflow / k_channels, std::memory_order_relaxed);
        const size_t write = (read + count) % cap;
        const size_t first = std::min(samples, cap - write);
        std::memcpy(buf.data() + write, src, first * sizeof(int16_t));
        if (samples > first)
            std::memcpy(buf.data(), src + first, (samples - first) * sizeof(int16_t));
        count += samples;
    }

    size_t take(int16_t* dst, size_t frames)
    {
        std::lock_guard<std::mutex> lk(mu);
        size_t samples = std::min(frames * k_channels, count);
        samples -= samples % k_channels;
        if (samples == 0)
            return 0;
        const size_t cap = buf.size();
        const size_t first = std::min(samples, cap - read);
        std::memcpy(dst, buf.data() + read, first * sizeof(int16_t));
        if (samples > first)
            std::memcpy(dst + first, buf.data(), (samples - first) * sizeof(int16_t));
        read = (read + samples) % cap;
        count -= samples;
        return samples / k_channels;
    }

    size_t available()
    {
        std::lock_guard<std::mutex> lk(mu);
        return count / k_channels;
    }

    /// Drop oldest samples when a device runs ahead of the mixer clock.
    void trim(size_t max_frames)
    {
        std::lock_guard<std::mutex> lk(mu);
        const size_t cap = buf.size();
        const size_t keep = std::min(max_frames * k_channels, cap);
        if (count <= keep)
            return;
        size_t drop = count - keep;
        drop -= drop % k_channels;
        if (drop == 0)
            return;
        read = (read + drop) % cap;
        count -= drop;
        dropped.fetch_add(drop / k_channels, std::memory_order_relaxed);
    }
};

struct Session {
    std::uint64_t id = 0;
    std::uint32_t sample_rate = k_rate;
    std::uint32_t bits = 16;
    std::string input;
    std::string kind;
    std::string requested_mic;
    std::string requested_loop;
    std::string mic_name;
    std::string loop_name;
    std::shared_ptr<Levels> levels = std::make_shared<Levels>();
    std::shared_ptr<PcmRing> ring;
    std::unique_ptr<AudioInput> mic;
    std::unique_ptr<DesktopAudioCapture> loop;
#if defined(FEATURE_AUDIO_MIX) && FEATURE_AUDIO_MIX
    std::unique_ptr<MixedAudioCapture> mix;
#endif
    std::vector<std::shared_ptr<Leg>> legs;
    std::atomic<bool> mix_run{false};
    std::thread mixer;

    ~Session()
    {
        mix_run.store(false);
        if (mixer.joinable())
            mixer.join();
        for (const auto& leg : legs) {
            if (leg && leg->mic)
                leg->mic->stop();
            if (leg && leg->loop)
                leg->loop->stop();
        }
        if (mic)
            mic->stop();
        if (loop)
            loop->stop();
#if defined(FEATURE_AUDIO_MIX) && FEATURE_AUDIO_MIX
        if (mix)
            mix->stop();
#endif
    }
};

void mix_loop(Session* session)
{
    using clock = std::chrono::steady_clock;
    constexpr int k_tick_ms = 10;
    const size_t k_frames = std::max<size_t>(1, (static_cast<size_t>(session->sample_rate) * k_tick_ms) / 1000);
    const size_t k_slack = k_frames * 4;
    std::vector<int16_t> chunk(k_frames * k_channels);
    std::vector<int16_t> mixed(k_frames * k_channels);
    std::vector<float> sum(k_frames * k_channels);

    const auto primed = clock::now() + std::chrono::milliseconds(40);
    while (session->mix_run.load() && clock::now() < primed) {
        bool ready = !session->legs.empty();
        for (const auto& leg : session->legs) {
            if (leg->available() < k_frames)
                ready = false;
        }
        if (ready)
            break;
        std::this_thread::sleep_for(std::chrono::milliseconds(2));
    }

    int waited_ms = 0;
    while (session->mix_run.load()) {
        if (!session->mix_run.load())
            break;
        for (const auto& leg : session->legs)
            leg->trim(k_frames + k_slack);
        bool all_ready = !session->legs.empty();
        size_t fullest = 0;
        for (const auto& leg : session->legs) {
            const size_t n = leg->available();
            if (n > fullest)
                fullest = n;
            if (n < k_frames)
                all_ready = false;
        }
        // Nobody has a block yet. Do not invent a silence block — that is the
        // click in the stream and in the recording.
        if (fullest < k_frames) {
            std::this_thread::sleep_for(std::chrono::milliseconds(2));
            continue;
        }
        // A callback often lands as 20 ms. Wait for the short legs only while
        // the fullest one is still inside that window. Once it is a block
        // ahead, emit and keep that source on time; waiting again between
        // every block is what slowed the mix to a third of realtime.
        if (!all_ready && fullest < k_frames * 2 && waited_ms < 20) {
            std::this_thread::sleep_for(std::chrono::milliseconds(2));
            waited_ms += 2;
            continue;
        }
        waited_ms = 0;
        std::fill(sum.begin(), sum.end(), 0.f);
        for (const auto& leg : session->legs) {
            const size_t got = leg->take(chunk.data(), k_frames);
            if (got < k_frames)
                leg->underruns.fetch_add(1, std::memory_order_relaxed);
            const float gain = leg->gain.load(std::memory_order_relaxed);
            const size_t n = got * k_channels;
            for (size_t i = 0; i < n; ++i)
                sum[i] += static_cast<float>(chunk[i]) * gain;
        }
        for (size_t i = 0; i < k_frames * k_channels; ++i)
            mixed[i] = static_cast<int16_t>(std::clamp(sum[i], -32768.f, 32767.f));
        note_pcm(session->levels, session->ring, mixed.data(), k_frames);
    }
}

bool prepare_devices(const Spec& spec, std::string& err)
{
    if (spec.kind == "mic" || spec.kind == "mix") {
        const auto mics = enumerate_capture_devices();
        if (!device_named(mics, spec.mic, err)) {
            if (err == "no device")
                err = "no capture device";
            return false;
        }
    }
    if (spec.kind == "loopback" || spec.kind == "mix") {
        const auto loops = enumerate_loopback_devices();
        if (!device_named(loops, spec.loopback, err)) {
            if (err == "no device")
                err = "no loopback device";
            return false;
        }
    }
    return true;
}

std::unique_ptr<Session> open_bus(const Spec& spec, std::string& err)
{
    const auto mics = enumerate_capture_devices();
    const auto loops = enumerate_loopback_devices();
    for (const auto& want : spec.legs) {
        const auto& devs = want.kind == "mic" ? mics : loops;
        if (!device_named(devs, want.name, err))
            return nullptr;
    }
    auto session = std::make_unique<Session>();
    session->input = spec.input;
    session->kind = spec.kind;
    session->sample_rate = spec.sample_rate;
    session->bits = spec.bits;
    session->ring = std::make_shared<PcmRing>(spec.sample_rate);
    try {
        for (const auto& want : spec.legs) {
            auto leg = std::make_shared<Leg>();
            leg->spec = want.spec;
            leg->kind = want.kind;
            leg->requested = want.name;
            leg->gain.store(want.gain);
            std::weak_ptr<Leg> weak = leg;
            auto on_pcm = [weak](const int16_t* pcm, size_t frames) {
                if (const auto held = weak.lock())
                    held->push(pcm, frames);
            };
            if (want.kind == "mic") {
                leg->mic = std::make_unique<AudioInput>();
                leg->mic->start(on_pcm, want.name, spec.sample_rate, k_channels, false);
                if (!leg->mic->is_running()) {
                    err = "microphone capture failed";
                    return nullptr;
                }
                leg->opened = leg->mic->opened_device_name();
            } else {
                leg->loop = std::make_unique<DesktopAudioCapture>();
                if (!leg->loop->start(on_pcm, want.name, err, spec.sample_rate, k_channels)) {
                    if (err.empty())
                        err = "loopback capture failed";
                    return nullptr;
                }
                leg->opened = leg->loop->opened_device_name();
            }
            if (leg->kind == "mic" && session->mic_name.empty())
                session->mic_name = leg->opened;
            if (leg->kind == "loopback" && session->loop_name.empty())
                session->loop_name = leg->opened;
            session->legs.push_back(std::move(leg));
        }
    } catch (const std::exception& ex) {
        err = ex.what();
        return nullptr;
    }
    session->mix_run.store(true);
    Session* raw = session.get();
    session->mixer = std::thread([raw] { mix_loop(raw); });
    return session;
}

std::unique_ptr<Session> open_session(const Spec& spec, std::string& err)
{
    if (!spec.legs.empty())
        return open_bus(spec, err);
    if (!prepare_devices(spec, err))
        return nullptr;
    auto session = std::make_unique<Session>();
    session->input = spec.input;
    session->kind = spec.kind;
    session->requested_mic = spec.mic;
    session->requested_loop = spec.loopback;
    session->sample_rate = spec.sample_rate;
    session->bits = spec.bits;
    session->ring = std::make_shared<PcmRing>(spec.sample_rate);
    try {
        if (spec.kind == "mic") {
            session->mic = std::make_unique<AudioInput>();
            auto levels = session->levels;
            auto ring = session->ring;
            session->mic->start(
                [levels, ring](const int16_t* pcm, size_t frames) {
                    note_pcm(levels, ring, pcm, frames);
                },
                spec.mic, spec.sample_rate, k_channels, false);
            if (!session->mic->is_running()) {
                err = "microphone capture failed";
                return nullptr;
            }
            session->mic_name = session->mic->opened_device_name();
            return session;
        }
        if (spec.kind == "loopback") {
            session->loop = std::make_unique<DesktopAudioCapture>();
            auto levels = session->levels;
            auto ring = session->ring;
            if (!session->loop->start(
                    [levels, ring](const int16_t* pcm, size_t frames) {
                        note_pcm(levels, ring, pcm, frames);
                    },
                    spec.loopback, err, spec.sample_rate, k_channels)) {
                if (err.empty())
                    err = "loopback capture failed";
                return nullptr;
            }
            session->loop_name = session->loop->opened_device_name();
            return session;
        }
#if defined(FEATURE_AUDIO_MIX) && FEATURE_AUDIO_MIX
        session->mix = std::make_unique<MixedAudioCapture>();
        auto levels = session->levels;
        auto ring = session->ring;
        auto cfg = make_recording_capture_config(
            "mix", spec.mic, spec.loopback, 1.0f, 1.0f, CaptureSource::mix);
        cfg.sample_rate = spec.sample_rate;
        cfg.channels = k_channels;
        if (!session->mix->start(
                cfg,
                [levels, ring](const int16_t* pcm, size_t frames) {
                    note_pcm(levels, ring, pcm, frames);
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
    if (!spec.legs.empty() || !live.legs.empty()) {
        if (spec.legs.size() != live.legs.size())
            return false;
        if (live.sample_rate != spec.sample_rate || live.bits != spec.bits)
            return false;
        for (size_t i = 0; i < spec.legs.size(); ++i) {
            if (spec.legs[i].spec != live.legs[i]->spec)
                return false;
        }
        return true;
    }
    if (live.sample_rate != spec.sample_rate || live.bits != spec.bits)
        return false;
    return live.kind == spec.kind && live.requested_mic == spec.mic
           && live.requested_loop == spec.loopback;
}

void apply_gains(Session& live, const Spec& spec)
{
    for (const auto& want : spec.legs) {
        for (const auto& leg : live.legs) {
            if (leg->spec == want.spec)
                leg->gain.store(want.gain);
        }
    }
}

void collect_devices(const Session& live, std::vector<std::pair<std::string, std::string>>& out)
{
    if (!live.legs.empty()) {
        for (const auto& leg : live.legs)
            out.emplace_back(leg->kind, leg->opened.empty() ? leg->requested : leg->opened);
        return;
    }
    if (live.kind == "mic" || live.kind == "mix")
        out.emplace_back("mic", live.mic_name.empty() ? live.requested_mic : live.mic_name);
    if (live.kind == "loopback" || live.kind == "mix")
        out.emplace_back("loopback", live.loop_name.empty() ? live.requested_loop : live.loop_name);
}

void collect_devices(const Spec& spec, std::vector<std::pair<std::string, std::string>>& out)
{
    if (!spec.legs.empty()) {
        for (const auto& leg : spec.legs)
            out.emplace_back(leg.kind, leg.name);
        return;
    }
    if (spec.kind == "mic" || spec.kind == "mix")
        out.emplace_back("mic", spec.mic);
    if (spec.kind == "loopback" || spec.kind == "mix")
        out.emplace_back("loopback", spec.loopback);
}

bool shares_device(const Session& live, const Spec& spec)
{
    std::vector<std::pair<std::string, std::string>> opened;
    std::vector<std::pair<std::string, std::string>> wanted;
    collect_devices(live, opened);
    collect_devices(spec, wanted);
    for (const auto& have : opened) {
        for (const auto& need : wanted) {
            if (have.first != need.first)
                continue;
            if (have.second.empty() || need.second.empty() || have.second == need.second)
                return true;
            if (icontains(have.second, need.second) || icontains(need.second, have.second))
                return true;
        }
    }
    return false;
}

} // namespace

struct SourceHost::Impl {
    std::mutex control;
    mutable std::mutex mu;
    Spec pending;
    std::unique_ptr<Session> live;
    std::uint64_t seq = 0;

    nlohmann::json status_unlocked() const
    {
        const bool running = static_cast<bool>(live);
        const int64_t now = steady_ns();
        const uint32_t peak = running
            ? fresh_peak(live->levels->peak.load(std::memory_order_relaxed),
                         live->levels->peak_at.load(std::memory_order_relaxed), now)
            : 0;
        const int64_t last = running ? live->levels->last_ns.load(std::memory_order_relaxed) : 0;
        const bool stalled = running && (last == 0 || now - last > 500'000'000);
        nlohmann::json out = {
            {"ok", true},
            {"running", running},
            {"session", running ? live->id : 0},
            {"phase", running ? "capture" : "idle"},
            {"input", running ? live->input : pending.input},
            {"kind", running ? live->kind : pending.kind},
            {"frames", running ? live->levels->frames.load(std::memory_order_relaxed) : 0},
            {"peak", peak},
            {"stalled", stalled},
            {"dropped", running && live->ring ? live->ring->dropped.load(std::memory_order_relaxed) : 0},
            {"sampleRate", running ? live->sample_rate : 0},
            {"bits", running ? live->bits : 0},
            {"channels", running ? k_channels : 0},
            {"mic", running ? live->mic_name : std::string{}},
            {"loopback", running ? live->loop_name : std::string{}},
#if defined(FEATURE_AUDIO_MIX) && FEATURE_AUDIO_MIX
            {"mix", true},
            {"bus", true},
#else
            {"mix", false},
            {"bus", false},
#endif
        };
        nlohmann::json rows = nlohmann::json::array();
        if (running) {
            for (const auto& leg : live->legs) {
                const int64_t heard = leg->last_ns.load(std::memory_order_relaxed);
                rows.push_back({
                    {"spec", leg->spec},
                    {"kind", leg->kind},
                    {"name", leg->opened.empty() ? leg->requested : leg->opened},
                    {"gain", leg->gain.load()},
                    {"peak", fresh_peak(leg->peak.load(std::memory_order_relaxed),
                                         leg->peak_at.load(std::memory_order_relaxed), now)},
                    {"dropped", leg->dropped.load(std::memory_order_relaxed)},
                    {"underruns", leg->underruns.load(std::memory_order_relaxed)},
                    {"stalled", heard == 0 || now - heard > 500'000'000},
                });
            }
        }
        out["sources"] = std::move(rows);
        return out;
    }

    bool start_spec(const Spec& spec, std::string& err, bool& already, bool& switched, bool& gains_only)
    {
        already = false;
        switched = false;
        gains_only = false;
        {
            std::lock_guard<std::mutex> lk(mu);
            if (live && same_request(*live, spec)) {
                if (!spec.legs.empty())
                    apply_gains(*live, spec);
                pending = spec;
                already = true;
                gains_only = !spec.legs.empty();
                return true;
            }
        }
        bool overlap = false;
        {
            std::lock_guard<std::mutex> lk(mu);
            overlap = live && shares_device(*live, spec);
            pending = spec;
        }
        if (overlap) {
            auto held = release();
            held.reset();
        }
        auto next = open_session(spec, err);
        if (!next)
            return false;
        std::unique_ptr<Session> old;
        std::shared_ptr<PcmRing> dropped;
        {
            std::lock_guard<std::mutex> lk(mu);
            if (live && same_request(*live, spec)) {
                if (!spec.legs.empty())
                    apply_gains(*live, spec);
                pending = spec;
                already = true;
                gains_only = !spec.legs.empty();
                old = std::move(next);
            } else {
                switched = static_cast<bool>(live);
                old = std::move(live);
                next->id = ++seq;
                live = std::move(next);
                pending = spec;
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

    std::shared_ptr<PcmRing> ring_for(uint64_t session)
    {
        std::lock_guard<std::mutex> lk(mu);
        if (!live || !live->ring)
            return {};
        if (session != 0 && live->id != session)
            return {};
        return live->ring;
    }

    uint64_t subscribe(uint64_t session)
    {
        const auto ring = ring_for(session);
        if (!ring)
            return 0;
        return add_cursor(ring);
    }

    void unsubscribe(uint64_t session, uint64_t cursor)
    {
        remove_cursor(ring_for(session), cursor);
    }

    int pull_cursor(uint64_t session, uint64_t cursor, int16_t* dst, int max_frames, int wait_ms)
    {
        const auto ring = ring_for(session);
        if (!ring)
            return -1;
        return read_cursor(ring, cursor, dst, max_frames, wait_ms);
    }

    int pull_session(uint64_t session, int16_t* dst, int max_frames, int wait_ms)
    {
        const auto ring = ring_for(session);
        if (!ring)
            return -1;
        uint64_t cursor = 0;
        {
            std::lock_guard<std::mutex> lk(ring->mu);
            cursor = ring->legacy;
        }
        if (cursor == 0)
            cursor = add_cursor(ring);
        {
            std::lock_guard<std::mutex> lk(ring->mu);
            if (ring->legacy == 0)
                ring->legacy = cursor;
            else
                cursor = ring->legacy;
        }
        return read_cursor(ring, cursor, dst, max_frames, wait_ms);
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
    std::lock_guard<std::mutex> gate(impl_->control);
    auto old = impl_->release();
    old.reset();
}

bool SourceHost::stop_session(std::uint64_t session)
{
    std::lock_guard<std::mutex> gate(impl_->control);
    {
        std::lock_guard<std::mutex> lk(impl_->mu);
        if (!impl_->live)
            return session == 0;
        if (session != 0 && impl_->live->id != session)
            return false;
    }
    auto old = impl_->release();
    old.reset();
    return true;
}

void SourceHost::set_capture_format(std::uint32_t sample_rate, std::uint32_t bits)
{
    if (sample_rate == 16000 || sample_rate == 24000 || sample_rate == k_rate)
        g_default_rate.store(sample_rate);
    if (bits == 16)
        g_default_bits.store(bits);
}

int SourceHost::pull(std::int16_t* dst, int max_frames, int wait_ms)
{
    return impl_->pull_session(0, dst, max_frames, wait_ms);
}

int SourceHost::pull_session(std::uint64_t session, std::int16_t* dst, int max_frames, int wait_ms)
{
    return impl_->pull_session(session, dst, max_frames, wait_ms);
}

std::uint64_t SourceHost::subscribe(std::uint64_t session)
{
    return impl_->subscribe(session);
}

void SourceHost::unsubscribe(std::uint64_t session, std::uint64_t cursor)
{
    impl_->unsubscribe(session, cursor);
}

int SourceHost::pull_cursor(std::uint64_t session, std::uint64_t cursor, std::int16_t* dst, int max_frames,
                            int wait_ms)
{
    return impl_->pull_cursor(session, cursor, dst, max_frames, wait_ms);
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
        {"bus", true},
#else
        {"mix", false},
        {"bus", false},
#endif
    };
    std::lock_guard<std::mutex> lk(impl_->mu);
    out["current"] = impl_->live ? impl_->live->input : impl_->pending.input;
    return {200, std::move(out)};
}

bool decode_request(const nlohmann::json& body,
                    const Spec& pending,
                    Spec& spec,
                    std::string& err)
{
    std::string input;
    std::string mic;
    std::string loopback;
    if (!read_fields(body, input, mic, loopback, err))
        return false;
    if (!parse_leg_list(body, spec.legs, err))
        return false;
    if (!spec.legs.empty()) {
        fill_bus(spec);
        return read_format(body, spec, false, err);
    }
    if (trim_copy(input).empty() && trim_copy(mic).empty() && trim_copy(loopback).empty()) {
        if (pending.input.empty() && pending.legs.empty()) {
            err = "missing input";
            return false;
        }
        spec = pending;
        return read_format(body, spec, true, err);
    }
    if (!parse_spec(input, mic, loopback, spec, err))
        return false;
    return read_format(body, spec, false, err);
}

SourceReply SourceHost::start(const nlohmann::json& body)
{
    std::lock_guard<std::mutex> gate(impl_->control);
    Spec pending;
    {
        std::lock_guard<std::mutex> lk(impl_->mu);
        pending = impl_->pending;
    }
    Spec spec;
    std::string err;
    if (!decode_request(body, pending, spec, err))
        return bad_request(err);
    bool already = false;
    bool switched = false;
    bool gains_only = false;
    if (!impl_->start_spec(spec, err, already, switched, gains_only)) {
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
    reply.body["gains"] = gains_only;
    return reply;
}

SourceReply SourceHost::select(const nlohmann::json& body)
{
    std::lock_guard<std::mutex> gate(impl_->control);
    Spec pending;
    bool running = false;
    {
        std::lock_guard<std::mutex> lk(impl_->mu);
        pending = impl_->pending;
        running = static_cast<bool>(impl_->live);
    }
    Spec spec;
    std::string err;
    if (!decode_request(body, pending, spec, err))
        return bad_request(err);
    if (!running) {
        std::lock_guard<std::mutex> lk(impl_->mu);
        impl_->pending = spec;
    }
    bool already = false;
    bool switched = false;
    bool gains_only = false;
    if (running && !impl_->start_spec(spec, err, already, switched, gains_only)) {
        log::trace("[audio-source] switch failed input=" + spec.input + " err=" + err);
        return bad_request(err, spec.input);
    }
    auto reply = status();
    reply.body["accepted"] = true;
    reply.body["switched"] = switched;
    reply.body["gains"] = gains_only;
    reply.body["stored"] = !running;
    return reply;
}

} // namespace pm::audio
