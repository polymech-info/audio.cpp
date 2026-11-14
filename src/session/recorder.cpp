#include "pm/audio/session/recorder.hpp"

#include "pm/audio/echo.hpp"
#include "pm/audio/log.hpp"
#include "pm/audio/session/aaudio_capture.hpp"
#include "pm/audio/session/audio_lease.hpp"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <fstream>
#include <mutex>
#include <thread>
#include <vector>

#if defined(PM_AUDIO_HAS_ONNX) && PM_AUDIO_HAS_ONNX
#include "pm/audio/filter/gtcrn.hpp"
#endif

namespace pm::audio::session {
namespace {

constexpr int k_speech_rate = 16000;
constexpr int k_hq_rate = 48000;

struct ToRate {
    int source = k_speech_rate;
    int target = k_speech_rate;
    std::int16_t prev = 0;
    bool has = false;
    double next = 0;
    std::int64_t seen = 0;

    void reset(int opened, int file_rate)
    {
        target = file_rate > 0 ? file_rate : k_speech_rate;
        source = opened > 0 ? opened : target;
        prev = 0;
        has = false;
        next = 0;
        seen = 0;
    }

    void push(const std::int16_t* in, std::size_t n, std::vector<std::int16_t>& out)
    {
        if (!in || n == 0)
            return;
        if (source == target) {
            out.insert(out.end(), in, in + n);
            seen += static_cast<std::int64_t>(n);
            return;
        }
        const double step = static_cast<double>(source) / static_cast<double>(target);
        for (std::size_t i = 0; i < n; ++i) {
            const std::int16_t cur = in[i];
            while (next < static_cast<double>(seen) + 1.0) {
                if (!has) {
                    out.push_back(cur);
                } else {
                    double frac = next - static_cast<double>(seen);
                    if (frac < 0)
                        frac = 0;
                    if (frac > 1)
                        frac = 1;
                    const double mixed = static_cast<double>(prev) * (1.0 - frac)
                        + static_cast<double>(cur) * frac;
                    out.push_back(static_cast<std::int16_t>(std::clamp(mixed, -32768.0, 32767.0)));
                }
                next += step;
            }
            prev = cur;
            has = true;
            seen += 1;
        }
    }
};

struct WavOut {
    std::ofstream file;
    std::uint32_t frames = 0;
    int rate = k_speech_rate;

    bool open(const std::string& path, int sample_rate)
    {
        rate = sample_rate > 0 ? sample_rate : k_speech_rate;
        file.open(path, std::ios::binary | std::ios::trunc);
        if (!file)
            return false;
        unsigned char hdr[44] = {};
        file.write(reinterpret_cast<const char*>(hdr), 44);
        frames = 0;
        return static_cast<bool>(file);
    }

    void write(const std::int16_t* pcm, std::size_t n)
    {
        if (!file || n == 0 || !pcm)
            return;
        file.write(reinterpret_cast<const char*>(pcm), static_cast<std::streamsize>(n * sizeof(std::int16_t)));
        frames += static_cast<std::uint32_t>(n);
    }

    bool close()
    {
        if (!file)
            return false;
        const std::uint32_t data_bytes = frames * static_cast<std::uint32_t>(sizeof(std::int16_t));
        unsigned char hdr[44];
        std::memset(hdr, 0, sizeof(hdr));
        hdr[0] = 'R'; hdr[1] = 'I'; hdr[2] = 'F'; hdr[3] = 'F';
        put(hdr + 4, 36u + data_bytes);
        hdr[8] = 'W'; hdr[9] = 'A'; hdr[10] = 'V'; hdr[11] = 'E';
        hdr[12] = 'f'; hdr[13] = 'm'; hdr[14] = 't'; hdr[15] = ' ';
        put(hdr + 16, 16);
        hdr[20] = 1; hdr[21] = 0;
        hdr[22] = 1; hdr[23] = 0;
        put(hdr + 24, static_cast<std::uint32_t>(rate));
        put(hdr + 28, static_cast<std::uint32_t>(rate) * 2u);
        hdr[32] = 2; hdr[33] = 0;
        hdr[34] = 16; hdr[35] = 0;
        hdr[36] = 'd'; hdr[37] = 'a'; hdr[38] = 't'; hdr[39] = 'a';
        put(hdr + 40, data_bytes);
        file.seekp(0);
        file.write(reinterpret_cast<const char*>(hdr), 44);
        const bool ok = static_cast<bool>(file);
        file.close();
        return ok;
    }

    static void put(unsigned char* dst, std::uint32_t value)
    {
        dst[0] = static_cast<unsigned char>(value & 0xffu);
        dst[1] = static_cast<unsigned char>((value >> 8) & 0xffu);
        dst[2] = static_cast<unsigned char>((value >> 16) & 0xffu);
        dst[3] = static_cast<unsigned char>((value >> 24) & 0xffu);
    }
};

void measure(const std::int16_t* pcm, std::size_t n, int& rms, int& peak, std::array<int, 16>& bands)
{
    if (!pcm || n == 0) {
        rms = 0;
        peak = 0;
        bands.fill(0);
        return;
    }
    long double sum = 0;
    int max_abs = 0;
    for (std::size_t i = 0; i < n; ++i) {
        const int sample = pcm[i];
        const int mag = sample < 0 ? -sample : sample;
        if (mag > max_abs)
            max_abs = mag;
        sum += static_cast<long double>(sample) * static_cast<long double>(sample);
    }
    rms = static_cast<int>(std::sqrt(static_cast<double>(sum / static_cast<long double>(n))));
    peak = max_abs;
    const std::size_t use = std::min(n, static_cast<std::size_t>(256));
    for (int b = 0; b < 16; ++b) {
        double re = 0;
        double im = 0;
        const double w = 2.0 * 3.14159265358979323846 * static_cast<double>(b + 1) * 4.0
            / static_cast<double>(use);
        for (std::size_t i = 0; i < use; ++i) {
            const double s = pcm[i];
            re += s * std::cos(w * static_cast<double>(i));
            im += s * std::sin(w * static_cast<double>(i));
        }
        const double mag = std::sqrt(re * re + im * im) / static_cast<double>(use);
        bands[static_cast<std::size_t>(b)] = static_cast<int>(std::min(32767.0, mag));
    }
}

struct Engine {
    std::mutex mu;
    RecorderStatus status;
    std::atomic<bool> stop{false};
    std::atomic<bool> want_gtcrn{false};
    std::atomic<bool> want_aec{false};
    std::atomic<bool> want_agc{false};
    std::atomic<bool> tap_on{false};
    std::mutex tap_mu;
    std::vector<std::int16_t> tap_pcm;
    std::thread worker;
    std::uint64_t token = 0;
    std::string model;
    bool alive = false;

    RecorderStatus copy()
    {
        std::lock_guard<std::mutex> lock(mu);
        return status;
    }

    void publish(const RecorderStatus& next)
    {
        std::lock_guard<std::mutex> lock(mu);
        status = next;
    }

    void tap_push(const std::int16_t* pcm, std::size_t n)
    {
        if (!pcm || n == 0 || !tap_on.load(std::memory_order_relaxed))
            return;
        constexpr std::size_t cap = 16000 * 3;
        std::lock_guard<std::mutex> lock(tap_mu);
        if (tap_pcm.size() + n > cap) {
            const std::size_t drop = tap_pcm.size() + n - cap;
            tap_pcm.erase(tap_pcm.begin(), tap_pcm.begin() + static_cast<std::ptrdiff_t>(drop));
        }
        tap_pcm.insert(tap_pcm.end(), pcm, pcm + n);
    }

    std::size_t tap_pull(std::int16_t* dst, std::size_t max)
    {
        if (!dst || max == 0)
            return 0;
        std::lock_guard<std::mutex> lock(tap_mu);
        const std::size_t n = std::min(max, tap_pcm.size());
        if (n == 0)
            return 0;
        std::memcpy(dst, tap_pcm.data(), n * sizeof(std::int16_t));
        tap_pcm.erase(tap_pcm.begin(), tap_pcm.begin() + static_cast<std::ptrdiff_t>(n));
        return n;
    }

    void tap_clear()
    {
        std::lock_guard<std::mutex> lock(tap_mu);
        tap_pcm.clear();
    }

    void run(int opened_rate, int file_rate)
    {
        ToRate resample;
        resample.reset(opened_rate, file_rate);
        WavOut wav;
        RecorderStatus local = copy();
        std::string path;
        {
            std::lock_guard<std::mutex> lock(mu);
            path = path_;
        }
        if (!wav.open(path, file_rate)) {
            local.ok = false;
            local.running = false;
            local.reason = "couldn't write the recording";
            publish(local);
            aaudio_capture_stop();
            return;
        }

#if defined(PM_AUDIO_HAS_ONNX) && PM_AUDIO_HAS_ONNX
        pm::audio::filter::gtcrn::StreamSession gtcrn;
        std::string gtcrn_err;
        bool gtcrn_open = false;
#else
        std::string gtcrn_err = "Noise remover isn't in this build";
        bool gtcrn_open = false;
#endif
        std::unique_ptr<pm::audio::EchoCancellationProcessor> apm;
        const std::size_t chunk = static_cast<std::size_t>(std::max(160, opened_rate / 10));
        std::vector<std::int16_t> raw(chunk);
        std::vector<std::int16_t> pcm16;
        std::vector<std::int16_t> enhanced;
        std::vector<std::int16_t> tap_down;
        ToRate tap_rate;
        bool tap_live = false;

        local.ok = true;
        local.running = true;
        local.opened_rate = opened_rate;
        local.file_rate = file_rate;
        local.reason = opened_rate == file_rate
            ? "Mic and file are " + std::to_string(file_rate) + " Hz"
            : "Mic opened at " + std::to_string(opened_rate) + " Hz. The file is "
                + std::to_string(file_rate) + " Hz.";
        publish(local);

        while (!stop.load(std::memory_order_relaxed)) {
            if (lease_token() != token) {
                local.reason = "audio lease lost";
                break;
            }
            int rate = opened_rate;
            const std::size_t got = aaudio_capture_read(raw.data(), raw.size(), &rate);
            if (got == 0) {
                std::this_thread::sleep_for(std::chrono::milliseconds(8));
                continue;
            }
            local.frames_in += static_cast<std::int64_t>(got);
            pcm16.clear();
            resample.push(raw.data(), got, pcm16);
            if (pcm16.empty())
                continue;

            const bool aec = want_aec.load(std::memory_order_relaxed);
            const bool agc = want_agc.load(std::memory_order_relaxed);
            local.aec_requested = aec;
            local.agc_requested = agc;
            const bool speech_rate = file_rate == k_speech_rate;
            if ((aec || agc) && speech_rate && !apm) {
                pm::audio::EchoCancellationProcessor::Config cfg;
                cfg.sample_rate_hz = k_speech_rate;
                cfg.channels = 1;
                cfg.agc = agc;
                cfg.ns = false;
                apm = pm::audio::EchoCancellationProcessor::create(cfg);
            }
            local.aec_active = apm && apm->active();
            if ((aec || agc) && !local.aec_active)
                local.filter_note = "Volume leveling isn't available on this phone.";
            else if (local.filter_note.find("Volume leveling") != std::string::npos)
                local.filter_note.clear();
            if (local.aec_active && (aec || agc))
                apm->process_capture(pcm16.data(), pcm16.size());

            const std::int16_t* write_pcm = pcm16.data();
            std::size_t write_n = pcm16.size();
            local.gtcrn_requested = want_gtcrn.load(std::memory_order_relaxed);
            local.last_process_ns = 0;
#if defined(PM_AUDIO_HAS_ONNX) && PM_AUDIO_HAS_ONNX
            if (local.gtcrn_requested && speech_rate && !gtcrn_open && gtcrn_err.empty()) {
                std::string err;
                gtcrn_open = gtcrn.open(model, err);
                if (!gtcrn_open) {
                    if (!err.empty())
                        pm::audio::log::warn(std::string("[recorder] noise remover: ") + err);
                    gtcrn_err = "Noise remover didn't start";
                }
            }
            if (local.gtcrn_requested && speech_rate && gtcrn_open) {
                enhanced.clear();
                const auto t0 = std::chrono::steady_clock::now();
                gtcrn.process(pcm16.data(), pcm16.size(), enhanced);
                const auto dt = std::chrono::steady_clock::now() - t0;
                local.last_process_ns = std::chrono::duration_cast<std::chrono::nanoseconds>(dt).count();
                local.process_ns += local.last_process_ns;
                write_pcm = enhanced.data();
                write_n = enhanced.size();
            }
#endif
            local.gtcrn_active = local.gtcrn_requested && speech_rate && gtcrn_open;
            if (!speech_rate && (local.gtcrn_requested || aec || agc))
                local.filter_note = "These filters save at 16 kHz. They apply to the next take.";
            else if (local.gtcrn_requested && !gtcrn_open)
                local.filter_note = gtcrn_err;
            else if (local.gtcrn_active && local.filter_note.find("Noise remover") != std::string::npos)
                local.filter_note.clear();

            if (write_n > 0) {
                measure(write_pcm, write_n, local.rms, local.peak, local.bands);
                wav.write(write_pcm, write_n);
                local.frames_written += static_cast<std::int64_t>(write_n);
                if (tap_on.load(std::memory_order_relaxed)) {
                    if (!tap_live) {
                        tap_rate.reset(file_rate, k_speech_rate);
                        tap_live = true;
                    }
                    if (file_rate == k_speech_rate) {
                        tap_push(write_pcm, write_n);
                    } else {
                        tap_down.clear();
                        tap_rate.push(write_pcm, write_n, tap_down);
                        tap_push(tap_down.data(), tap_down.size());
                    }
                } else {
                    tap_live = false;
                }
            }
            const auto cap = aaudio_capture_status();
            local.dropped = cap.dropped;
            if (cap.device_lost) {
                local.reason = "selected device lost";
                break;
            }
            publish(local);
        }

        wav.close();
        aaudio_capture_stop();
        local.running = false;
        local.ok = local.reason != "couldn't write the recording";
        if (local.reason.rfind("Mic and file are ", 0) == 0
            || local.reason.rfind("Mic opened at ", 0) == 0)
            local.reason = "saved";
        publish(local);
    }

    std::string path_;
};

Engine& engine()
{
    static Engine instance;
    return instance;
}

} // namespace

RecorderStatus recorder_start(const RecorderRequest& request)
{
    Engine& eng = engine();
    if (eng.alive) {
        RecorderStatus busy;
        busy.running = true;
        busy.reason = "already recording";
        return busy;
    }
    if (eng.worker.joinable())
        eng.worker.join();
    if (request.token == 0 || request.token != lease_token()) {
        RecorderStatus denied;
        denied.reason = "capture requires a live audio lease";
        return denied;
    }
    if (!lease_allows_capture(lease_holder())) {
        RecorderStatus denied;
        denied.reason = "this lease cannot capture";
        return denied;
    }
    if (request.wav_path.empty()) {
        RecorderStatus denied;
        denied.reason = "recording path is empty";
        return denied;
    }

    const bool speech = request.gtcrn || request.aec || request.agc;
    const int file_rate = speech ? k_speech_rate : k_hq_rate;
    const AAudioStatus opened = aaudio_capture_start(
        request.token, -1, false, "recognition", file_rate);
    if (!opened.ok) {
        RecorderStatus denied;
        denied.reason = opened.reason.empty() ? "capture did not start" : opened.reason;
        return denied;
    }

    eng.stop.store(false);
    eng.want_gtcrn.store(request.gtcrn);
    eng.want_aec.store(request.aec);
    eng.want_agc.store(request.agc);
    eng.token = request.token;
    eng.model = request.model_path;
    eng.path_ = request.wav_path;
    eng.alive = true;
    {
        RecorderStatus starting;
        starting.ok = true;
        starting.running = true;
        starting.reason = "opened";
        starting.opened_rate = opened.sample_rate;
        starting.file_rate = file_rate;
        starting.gtcrn_requested = request.gtcrn;
        starting.aec_requested = request.aec;
        starting.agc_requested = request.agc;
        eng.publish(starting);
    }
    const int rate = opened.sample_rate > 0 ? opened.sample_rate : file_rate;
    eng.worker = std::thread([&eng, rate, file_rate] { eng.run(rate, file_rate); });
    return eng.copy();
}

RecorderStatus recorder_set(bool gtcrn, bool aec, bool agc)
{
    Engine& eng = engine();
    eng.want_gtcrn.store(gtcrn);
    eng.want_aec.store(aec);
    eng.want_agc.store(agc);
    RecorderStatus status = eng.copy();
    status.gtcrn_requested = gtcrn;
    status.aec_requested = aec;
    status.agc_requested = agc;
    if (!status.running && !eng.alive) {
        status.ok = true;
        status.reason = "idle";
    }
    return status;
}

RecorderStatus recorder_stop()
{
    Engine& eng = engine();
    eng.stop.store(true);
    if (eng.worker.joinable())
        eng.worker.join();
    eng.alive = false;
    RecorderStatus status = eng.copy();
    if (status.reason.empty())
        status.reason = "saved";
    status.running = false;
    return status;
}

RecorderStatus recorder_stats()
{
    return engine().copy();
}

void recorder_tap(bool on)
{
    Engine& eng = engine();
    eng.tap_on.store(on);
    if (!on)
        eng.tap_clear();
}

std::size_t recorder_pull_pcm(std::int16_t* dst, std::size_t max_samples)
{
    return engine().tap_pull(dst, max_samples);
}

} // namespace pm::audio::session
