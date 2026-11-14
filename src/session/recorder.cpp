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
#include <filesystem>
#include <fstream>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#if defined(PM_AUDIO_HAS_ONNX) && PM_AUDIO_HAS_ONNX
#include "pm/audio/filter/gtcrn.hpp"
#endif

namespace pm::audio::session {
namespace {

constexpr int k_speech_rate = 16000;
constexpr int k_hq_rate = 48000;
constexpr int k_apm_frame = k_speech_rate / 100;
constexpr double k_pi = 3.14159265358979323846;

struct RateConv {
    static constexpr int k_wing = 64;
    static constexpr int k_taps = k_wing * 2;
    static constexpr int k_phases = 512;

    int source = k_speech_rate;
    int target = k_speech_rate;
    double step = 1;
    double pos = 0;
    std::int64_t base = 0;
    std::vector<float> table;
    std::vector<std::int16_t> hist;

    static double sinc(double x)
    {
        if (std::fabs(x) < 1e-12)
            return 1.0;
        const double p = k_pi * x;
        return std::sin(p) / p;
    }

    static double window(double dist)
    {
        if (dist <= -k_wing || dist >= k_wing)
            return 0.0;
        const double t = (dist + k_wing) / (2.0 * k_wing);
        constexpr double a0 = 0.35875;
        constexpr double a1 = 0.48829;
        constexpr double a2 = 0.14128;
        constexpr double a3 = 0.01168;
        return a0 - a1 * std::cos(2.0 * k_pi * t) + a2 * std::cos(4.0 * k_pi * t)
            - a3 * std::cos(6.0 * k_pi * t);
    }

    void reset(int opened, int file_rate)
    {
        target = file_rate > 0 ? file_rate : k_speech_rate;
        source = opened > 0 ? opened : target;
        step = static_cast<double>(source) / static_cast<double>(target);
        pos = 0;
        base = 0;
        hist.clear();
        table.clear();
        if (source == target || step <= 0)
            return;
        const double scale = std::min(1.0, static_cast<double>(target) / static_cast<double>(source));
        table.assign(static_cast<std::size_t>(k_phases * k_taps), 0.f);
        for (int phase = 0; phase < k_phases; ++phase) {
            const double frac = static_cast<double>(phase) / static_cast<double>(k_phases);
            double sum = 0;
            float* row = table.data() + static_cast<std::size_t>(phase * k_taps);
            for (int tap = 0; tap < k_taps; ++tap) {
                const double dist = static_cast<double>(tap - (k_wing - 1)) - frac;
                const float coeff = static_cast<float>(scale * sinc(scale * dist) * window(dist));
                row[tap] = coeff;
                sum += coeff;
            }
            if (sum != 0.0) {
                for (int tap = 0; tap < k_taps; ++tap)
                    row[tap] = static_cast<float>(row[tap] / sum);
            }
        }
    }

    void emit_until(double limit, std::vector<std::int16_t>& out)
    {
        const std::int64_t end = base + static_cast<std::int64_t>(hist.size());
        while (pos < limit) {
            const auto center = static_cast<std::int64_t>(std::floor(pos));
            if (center + k_wing >= end)
                break;
            const double frac = pos - static_cast<double>(center);
            int phase = static_cast<int>(frac * static_cast<double>(k_phases));
            if (phase >= k_phases)
                phase = k_phases - 1;
            if (phase < 0)
                phase = 0;
            const float* row = table.data() + static_cast<std::size_t>(phase * k_taps);
            double acc = 0;
            for (int tap = 0; tap < k_taps; ++tap) {
                const std::int64_t idx = center + (tap - (k_wing - 1));
                if (idx < base || idx >= end)
                    continue;
                acc += static_cast<double>(hist[static_cast<std::size_t>(idx - base)]) * row[tap];
            }
            acc = std::clamp(acc, -32768.0, 32767.0);
            out.push_back(static_cast<std::int16_t>(std::lround(acc)));
            pos += step;
        }
        const auto keep = static_cast<std::int64_t>(std::floor(pos)) - (k_wing - 1);
        if (keep > base) {
            const auto drop = static_cast<std::size_t>(keep - base);
            if (drop >= hist.size()) {
                hist.clear();
                base = keep;
            } else {
                hist.erase(hist.begin(), hist.begin() + static_cast<std::ptrdiff_t>(drop));
                base = keep;
            }
        }
    }

    void push(const std::int16_t* in, std::size_t n, std::vector<std::int16_t>& out)
    {
        if (!in || n == 0)
            return;
        if (source == target || step <= 0 || table.empty()) {
            out.insert(out.end(), in, in + n);
            return;
        }
        hist.insert(hist.end(), in, in + n);
        emit_until(1.0e300, out);
    }

    void flush(std::vector<std::int16_t>& out)
    {
        if (source == target || step <= 0 || table.empty() || hist.empty())
            return;
        const double real_end = static_cast<double>(base + static_cast<std::int64_t>(hist.size()));
        hist.insert(hist.end(), static_cast<std::size_t>(k_wing), 0);
        emit_until(real_end, out);
    }

    static std::string check()
    {
        std::string err;
        auto fail = [&](const char* text) {
            if (!err.empty())
                err += "; ";
            err += text;
        };
        auto rms_of = [](const std::vector<std::int16_t>& pcm, std::size_t skip) {
            if (pcm.size() <= skip + 8)
                return 0.0;
            double sum = 0;
            const std::size_t end = pcm.size() - 4;
            for (std::size_t i = skip; i < end; ++i) {
                const double s = pcm[i];
                sum += s * s;
            }
            return std::sqrt(sum / static_cast<double>(end - skip));
        };
        auto tone = [](int rate, int freq, int n, int amp) {
            std::vector<std::int16_t> pcm(static_cast<std::size_t>(n));
            for (int i = 0; i < n; ++i) {
                const double s = std::sin(2.0 * k_pi * freq * i / rate) * amp;
                pcm[static_cast<std::size_t>(i)] = static_cast<std::int16_t>(std::lround(s));
            }
            return pcm;
        };
        {
            RateConv conv;
            conv.reset(16000, 16000);
            std::vector<std::int16_t> in(100);
            std::vector<std::int16_t> out;
            for (int i = 0; i < 100; ++i)
                in[static_cast<std::size_t>(i)] = static_cast<std::int16_t>(i - 50);
            conv.push(in.data(), in.size(), out);
            conv.flush(out);
            if (out != in)
                fail("same-rate resample changed the samples");
        }
        {
            const auto in = tone(48000, 1000, 4800, 10000);
            RateConv conv;
            conv.reset(48000, 16000);
            std::vector<std::int16_t> out;
            conv.push(in.data(), in.size(), out);
            conv.flush(out);
            const auto count = static_cast<int>(out.size());
            if (std::abs(count - 1600) > 2)
                fail("48 to 16 kHz sample count");
            if (rms_of(out, 80) < 6000)
                fail("1 kHz did not survive 48 to 16 kHz");
        }
        {
            const auto in = tone(48000, 12000, 4800, 10000);
            RateConv conv;
            conv.reset(48000, 16000);
            std::vector<std::int16_t> out;
            conv.push(in.data(), in.size(), out);
            conv.flush(out);
            if (rms_of(out, 80) > 200)
                fail("12 kHz aliased through 48 to 16 kHz");
        }
        {
            const auto in = tone(16000, 1000, 1600, 10000);
            RateConv conv;
            conv.reset(16000, 48000);
            std::vector<std::int16_t> out;
            conv.push(in.data(), in.size(), out);
            conv.flush(out);
            if (std::abs(static_cast<int>(out.size()) - 4800) > 2)
                fail("16 to 48 kHz sample count");
            if (rms_of(out, 80) < 6000)
                fail("1 kHz did not survive 16 to 48 kHz");
        }
        {
            std::vector<std::int16_t> in(3000);
            for (std::size_t i = 0; i < in.size(); ++i)
                in[i] = static_cast<std::int16_t>((static_cast<int>(i) * 17) % 2000 - 1000);
            RateConv whole;
            whole.reset(48000, 16000);
            std::vector<std::int16_t> a;
            whole.push(in.data(), in.size(), a);
            whole.flush(a);
            RateConv split;
            split.reset(48000, 16000);
            std::vector<std::int16_t> b;
            for (std::size_t i = 0; i < in.size(); i += 100)
                split.push(in.data() + i, std::min<std::size_t>(100, in.size() - i), b);
            split.flush(b);
            if (a != b)
                fail("resample changed across chunk boundaries");
        }
        return err;
    }
};

struct WavOut {
    static constexpr std::uint32_t k_max_frames = (0xFFFFFFFFu - 36u) / 2u;

    std::ofstream file;
    std::uint32_t frames = 0;
    int rate = k_speech_rate;
    bool failed = false;
    bool limited = false;

    ~WavOut()
    {
        if (file.is_open())
            close();
    }

    bool open(const std::string& path, int sample_rate)
    {
        rate = sample_rate > 0 ? sample_rate : k_speech_rate;
        failed = false;
        file.open(path, std::ios::binary | std::ios::trunc);
        if (!file) {
            failed = true;
            return false;
        }
        unsigned char hdr[44] = {};
        file.write(reinterpret_cast<const char*>(hdr), 44);
        frames = 0;
        if (!file) {
            failed = true;
            return false;
        }
        return true;
    }

    bool write(const std::int16_t* pcm, std::size_t n)
    {
        if (n == 0)
            return !failed;
        if (failed || !file.is_open() || !pcm)
            return false;
        if (n > k_max_frames - frames) {
            failed = true;
            limited = true;
            return false;
        }
        file.write(reinterpret_cast<const char*>(pcm),
                   static_cast<std::streamsize>(n * sizeof(std::int16_t)));
        if (!file) {
            failed = true;
            return false;
        }
        frames += static_cast<std::uint32_t>(n);
        return true;
    }

    bool close()
    {
        if (!file.is_open())
            return false;
        const bool io_failed = failed || !file;
        if (!file)
            file.clear();
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
        const bool header_ok = static_cast<bool>(file);
        file.close();
        return header_ok && !io_failed;
    }

    static void put(unsigned char* dst, std::uint32_t value)
    {
        dst[0] = static_cast<unsigned char>(value & 0xffu);
        dst[1] = static_cast<unsigned char>((value >> 8) & 0xffu);
        dst[2] = static_cast<unsigned char>((value >> 16) & 0xffu);
        dst[3] = static_cast<unsigned char>((value >> 24) & 0xffu);
    }

    static std::string check()
    {
        std::string err;
        auto fail = [&](const char* text) {
            if (!err.empty())
                err += "; ";
            err += text;
        };
        const auto dir = std::filesystem::temp_directory_path() / "pm-audio-recorder-wav";
        std::error_code ec;
        std::filesystem::remove_all(dir, ec);
        std::filesystem::create_directories(dir, ec);
        const auto path = dir / "take.wav";
        {
            WavOut wav;
            if (!wav.open(path.string(), 16000))
                fail("wav open");
            const std::int16_t pcm[4] = {1, -2, 3, -4};
            if (!wav.write(pcm, 4) || !wav.close())
                fail("wav write");
            std::ifstream in(path, std::ios::binary);
            unsigned char hdr[44];
            in.read(reinterpret_cast<char*>(hdr), 44);
            std::int16_t got[4] = {};
            in.read(reinterpret_cast<char*>(got), sizeof(got));
            const auto data = static_cast<std::uint32_t>(hdr[40] | (hdr[41] << 8) | (hdr[42] << 16) | (hdr[43] << 24));
            if (!in || data != 8 || got[0] != 1 || got[3] != -4)
                fail("wav header does not match the samples");
        }
        {
            WavOut wav;
            if (!wav.open(path.string(), 16000))
                fail("wav reopen");
            wav.file.setstate(std::ios::badbit);
            const std::int16_t one = 7;
            if (wav.write(&one, 1) || wav.close())
                fail("a failed wav write was reported as saved");
        }
        {
            WavOut wav;
            if (!wav.open(path.string(), 48000))
                fail("wav limit open");
            wav.frames = k_max_frames - 1;
            const std::int16_t extra[2] = {1, 2};
            if (wav.write(extra, 2))
                fail("wav size limit accepted another chunk");
            wav.close();
        }
        {
            WavOut wav;
            if (wav.close())
                fail("closing an unopened wav succeeded");
        }
        std::filesystem::remove_all(dir, ec);
        return err;
    }
};

void measure_levels(const std::int16_t* pcm, std::size_t n, int& rms, int& peak)
{
    std::uint64_t sum = 0;
    int max_abs = 0;
    for (std::size_t i = 0; i < n; ++i) {
        const int sample = pcm[i];
        const int mag = sample < 0 ? -sample : sample;
        if (mag > max_abs)
            max_abs = mag;
        sum += static_cast<std::uint64_t>(static_cast<std::int64_t>(sample) * sample);
    }
    rms = static_cast<int>(std::lround(std::sqrt(static_cast<double>(sum) / static_cast<double>(n))));
    peak = max_abs;
}

void measure_bands(const std::int16_t* pcm, std::size_t n, std::array<int, 16>& bands)
{
    const std::size_t use = std::min(n, static_cast<std::size_t>(256));
    if (use == 0) {
        bands.fill(0);
        return;
    }
    for (int b = 0; b < 16; ++b) {
        const double w = 2.0 * k_pi * static_cast<double>(b + 1) * 4.0 / static_cast<double>(use);
        const double coeff = 2.0 * std::cos(w);
        double s1 = 0;
        double s2 = 0;
        for (std::size_t i = 0; i < use; ++i) {
            const double s0 = static_cast<double>(pcm[i]) + coeff * s1 - s2;
            s2 = s1;
            s1 = s0;
        }
        const double cw = std::cos(w);
        const double sw = std::sin(w);
        const double re = s1 - s2 * cw;
        const double im = s2 * sw;
        const double mag = std::sqrt(re * re + im * im) / static_cast<double>(use);
        bands[static_cast<std::size_t>(b)] = static_cast<int>(std::min(32767.0, mag));
    }
}

struct PcmTap {
    static constexpr std::size_t cap = 16000 * 3;
    std::vector<std::int16_t> buf;
    std::size_t read = 0;
    std::size_t count = 0;

    PcmTap()
        : buf(cap)
    {
    }

    void clear()
    {
        read = 0;
        count = 0;
    }

    void push(const std::int16_t* pcm, std::size_t n)
    {
        if (!pcm || n == 0)
            return;
        if (n >= cap) {
            pcm += n - cap;
            n = cap;
            read = 0;
            count = 0;
        } else if (count + n > cap) {
            const std::size_t drop = count + n - cap;
            read = (read + drop) % cap;
            count -= drop;
        }
        const std::size_t write = (read + count) % cap;
        const std::size_t first = std::min(n, cap - write);
        std::memcpy(buf.data() + write, pcm, first * sizeof(std::int16_t));
        if (n > first)
            std::memcpy(buf.data(), pcm + first, (n - first) * sizeof(std::int16_t));
        count += n;
    }

    std::size_t pull(std::int16_t* dst, std::size_t max)
    {
        if (!dst || max == 0 || count == 0)
            return 0;
        const std::size_t n = std::min(max, count);
        const std::size_t first = std::min(n, cap - read);
        std::memcpy(dst, buf.data() + read, first * sizeof(std::int16_t));
        if (n > first)
            std::memcpy(dst + first, buf.data(), (n - first) * sizeof(std::int16_t));
        read = (read + n) % cap;
        count -= n;
        return n;
    }

    static std::string check()
    {
        PcmTap tap;
        std::vector<std::int16_t> big(PcmTap::cap + 10);
        for (std::size_t i = 0; i < big.size(); ++i)
            big[i] = static_cast<std::int16_t>(i);
        tap.push(big.data(), big.size());
        if (tap.count != PcmTap::cap)
            return "tap kept more than its cap";
        std::vector<std::int16_t> got(PcmTap::cap);
        const std::size_t n = tap.pull(got.data(), got.size());
        if (n != PcmTap::cap || got.front() != static_cast<std::int16_t>(10) || got.back() != static_cast<std::int16_t>(big.size() - 1))
            return "tap overflow did not keep the newest samples";
        return {};
    }
};

struct Engine {
    std::mutex life;
    std::mutex mu;
    RecorderStatus status;
    std::atomic<bool> stop{false};
    std::atomic<bool> want_gtcrn{false};
    std::atomic<bool> want_aec{false};
    std::atomic<bool> want_agc{false};
    std::atomic<bool> tap_on{false};
    std::atomic<bool> worker_done{true};
    std::mutex tap_mu;
    PcmTap tap;
    std::thread worker;

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
        std::lock_guard<std::mutex> lock(tap_mu);
        if (!tap_on.load(std::memory_order_relaxed))
            return;
        tap.push(pcm, n);
    }

    std::size_t tap_pull(std::int16_t* dst, std::size_t max)
    {
        std::lock_guard<std::mutex> lock(tap_mu);
        return tap.pull(dst, max);
    }

    void tap_clear()
    {
        std::lock_guard<std::mutex> lock(tap_mu);
        tap.clear();
    }

    void run(int opened_rate, int file_rate, std::string path, std::string model, std::uint64_t token)
    {
        try {
            run_body(opened_rate, file_rate, std::move(path), std::move(model), token);
        } catch (const std::exception& ex) {
            pm::audio::log::warn(std::string("[recorder] ") + ex.what());
            RecorderStatus failed = copy();
            failed.ok = false;
            failed.running = false;
            failed.reason = "recording stopped unexpectedly";
            publish(failed);
            aaudio_capture_stop();
        } catch (...) {
            RecorderStatus failed = copy();
            failed.ok = false;
            failed.running = false;
            failed.reason = "recording stopped unexpectedly";
            publish(failed);
            aaudio_capture_stop();
        }
        worker_done.store(true, std::memory_order_release);
    }

    void run_body(int opened_rate, int file_rate, std::string path, std::string model, std::uint64_t token)
    {
        struct StopCapture {
            ~StopCapture() { aaudio_capture_stop(); }
        } guard;

        RateConv resample;
        resample.reset(opened_rate, file_rate);
        WavOut wav;
        RecorderStatus local = copy();
        if (!wav.open(path, file_rate)) {
            local.ok = false;
            local.running = false;
            local.reason = "couldn't write the recording";
            publish(local);
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
        bool gtcrn_failed = false;
        std::unique_ptr<pm::audio::EchoCancellationProcessor> apm;
        int applied_agc = -1;
        std::vector<std::int16_t> apm_hold;
        const std::size_t full_chunk = static_cast<std::size_t>(std::max(160, opened_rate / 10));
        const std::size_t live_chunk = static_cast<std::size_t>(std::max(160, opened_rate / 50));
        std::vector<std::int16_t> raw(full_chunk);
        std::vector<std::int16_t> pcm16;
        std::vector<std::int16_t> shaped;
        std::vector<std::int16_t> enhanced;
        std::vector<std::int16_t> tap_down;
        RateConv tap_rate;
        bool tap_live = false;
        bool failed = false;
        bool rate_locked = false;
        int stream_rate = opened_rate;
        int silent_reads = 0;
        auto bands_at = std::chrono::steady_clock::time_point{};

        local.ok = true;
        local.running = true;
        local.opened_rate = opened_rate;
        local.file_rate = file_rate;
        local.reason = opened_rate == file_rate
            ? "Mic and file are " + std::to_string(file_rate) + " Hz"
            : "Mic opened at " + std::to_string(opened_rate) + " Hz. The file is "
                + std::to_string(file_rate) + " Hz.";
        publish(local);

        auto write_block = [&](const std::int16_t* pcm, std::size_t n) -> bool {
            if (n == 0 || !pcm)
                return true;
            const auto now = std::chrono::steady_clock::now();
            measure_levels(pcm, n, local.rms, local.peak);
            if (now >= bands_at) {
                measure_bands(pcm, n, local.bands);
                bands_at = now + std::chrono::milliseconds(80);
            }
            if (!wav.write(pcm, n))
                return false;
            local.frames_written += static_cast<std::int64_t>(n);
            if (tap_on.load(std::memory_order_relaxed)) {
                if (!tap_live) {
                    tap_rate.reset(file_rate, k_speech_rate);
                    tap_live = true;
                }
                if (file_rate == k_speech_rate) {
                    tap_push(pcm, n);
                } else {
                    tap_down.clear();
                    tap_rate.push(pcm, n, tap_down);
                    if (!tap_down.empty())
                        tap_push(tap_down.data(), tap_down.size());
                }
            } else {
                tap_live = false;
            }
            return true;
        };

        auto emit = [&](const std::int16_t* pcm, std::size_t n, bool drain) -> bool {
            const bool aec = want_aec.load(std::memory_order_relaxed);
            const bool agc = want_agc.load(std::memory_order_relaxed);
            local.aec_requested = aec;
            local.agc_requested = agc;
            const bool speech_rate = file_rate == k_speech_rate;
            const bool want_apm = (aec || agc) && speech_rate;
            if (!want_apm) {
                apm.reset();
                applied_agc = -1;
            } else if (applied_agc != (agc ? 1 : 0)) {
                pm::audio::EchoCancellationProcessor::Config cfg;
                cfg.sample_rate_hz = k_speech_rate;
                cfg.channels = 1;
                cfg.agc = agc;
                cfg.ns = false;
                apm = pm::audio::EchoCancellationProcessor::create(cfg);
                applied_agc = agc ? 1 : 0;
                if (apm)
                    apm->set_stream_delay_ms(0);
            }
            local.aec_active = static_cast<bool>(apm) && apm->active();
            if ((aec || agc) && !local.aec_active)
                local.filter_note = "Volume leveling isn't available on this phone.";
            else if (local.filter_note.find("Volume leveling") != std::string::npos)
                local.filter_note.clear();
            if (aec && speech_rate && local.filter_note.empty())
                local.filter_note = "Echo removal needs speaker audio, which this take does not have.";

            shaped.clear();
            if (n > 0 && pcm)
                apm_hold.insert(apm_hold.end(), pcm, pcm + n);
            if (local.aec_active && (aec || agc)) {
                while (apm_hold.size() >= static_cast<std::size_t>(k_apm_frame)) {
                    apm->process_capture(apm_hold.data(), static_cast<std::size_t>(k_apm_frame));
                    shaped.insert(shaped.end(), apm_hold.begin(), apm_hold.begin() + k_apm_frame);
                    apm_hold.erase(apm_hold.begin(), apm_hold.begin() + k_apm_frame);
                }
                if (drain && !apm_hold.empty()) {
                    std::vector<std::int16_t> pad(static_cast<std::size_t>(k_apm_frame), 0);
                    std::copy(apm_hold.begin(), apm_hold.end(), pad.begin());
                    apm->process_capture(pad.data(), pad.size());
                    shaped.insert(shaped.end(), pad.begin(), pad.begin() + static_cast<std::ptrdiff_t>(apm_hold.size()));
                    apm_hold.clear();
                }
            } else if (!apm_hold.empty() && (!want_apm || drain || !local.aec_active)) {
                shaped.insert(shaped.end(), apm_hold.begin(), apm_hold.end());
                apm_hold.clear();
            }

            const std::int16_t* block = shaped.data();
            std::size_t block_n = shaped.size();
            local.gtcrn_requested = want_gtcrn.load(std::memory_order_relaxed);
            // A failed open stays failed until the toggle drops, then the next on tries again.
            if (!local.gtcrn_requested && gtcrn_failed) {
                gtcrn_failed = false;
                gtcrn_err.clear();
            }
            local.last_process_ns = 0;
#if defined(PM_AUDIO_HAS_ONNX) && PM_AUDIO_HAS_ONNX
            if (local.gtcrn_requested && speech_rate && !gtcrn_open && gtcrn_err.empty()) {
                std::string open_err;
                gtcrn_open = gtcrn.open(model, open_err);
                if (!gtcrn_open) {
                    if (!open_err.empty())
                        pm::audio::log::warn(std::string("[recorder] noise remover: ") + open_err);
                    gtcrn_err = "Noise remover didn't start";
                    gtcrn_failed = true;
                }
            }
            if (local.gtcrn_requested && speech_rate && gtcrn_open && block_n > 0) {
                enhanced.clear();
                const auto t0 = std::chrono::steady_clock::now();
                gtcrn.process(block, block_n, enhanced);
                const auto dt = std::chrono::steady_clock::now() - t0;
                local.last_process_ns = std::chrono::duration_cast<std::chrono::nanoseconds>(dt).count();
                local.process_ns += local.last_process_ns;
                block = enhanced.data();
                block_n = enhanced.size();
            }
#else
            (void)model;
#endif
            local.gtcrn_active = local.gtcrn_requested && speech_rate && gtcrn_open;
            if (!speech_rate && (local.gtcrn_requested || aec || agc))
                local.filter_note = "These filters save at 16 kHz. They apply to the next take.";
            else if (local.gtcrn_requested && !gtcrn_open)
                local.filter_note = gtcrn_err;
            else if (local.gtcrn_active && local.filter_note.find("Noise remover") != std::string::npos)
                local.filter_note.clear();
            return write_block(block, block_n);
        };

        while (!stop.load(std::memory_order_relaxed)) {
            if (lease_token() != token) {
                failed = true;
                local.reason = "audio lease lost";
                break;
            }
            int rate = stream_rate;
            const bool live = tap_on.load(std::memory_order_relaxed);
            const std::size_t ask = live ? live_chunk : full_chunk;
            const std::size_t got = aaudio_capture_read(raw.data(), std::min(ask, raw.size()), &rate);
            if (got == 0) {
                const auto cap = aaudio_capture_status();
                local.dropped = cap.dropped;
                if (cap.device_lost) {
                    failed = true;
                    local.reason = "selected device lost";
                    break;
                }
                if (!cap.running) {
                    failed = true;
                    local.reason = "capture stopped";
                    break;
                }
                if (++silent_reads >= 250) {
                    failed = true;
                    local.reason = "capture produced no audio";
                    break;
                }
                std::this_thread::sleep_for(std::chrono::milliseconds(8));
                continue;
            }
            silent_reads = 0;
            if (rate > 0 && rate != stream_rate) {
                if (rate_locked) {
                    failed = true;
                    local.reason = "capture rate changed";
                    break;
                }
                stream_rate = rate;
                local.opened_rate = rate;
                resample.reset(stream_rate, file_rate);
            }
            rate_locked = true;
            local.frames_in += static_cast<std::int64_t>(got);
            pcm16.clear();
            resample.push(raw.data(), got, pcm16);
            if (!pcm16.empty() && !emit(pcm16.data(), pcm16.size(), false)) {
                failed = true;
                local.reason = wav.limited
                    ? "recording reached the WAV size limit"
                    : "couldn't write the recording";
                break;
            }
            const auto cap = aaudio_capture_status();
            local.dropped = cap.dropped;
            if (cap.device_lost) {
                failed = true;
                local.reason = "selected device lost";
                break;
            }
            publish(local);
        }

        if (!failed) {
            pcm16.clear();
            resample.flush(pcm16);
            if (!emit(pcm16.data(), pcm16.size(), true)) {
                failed = true;
                local.reason = "couldn't write the recording";
            }
        }
#if defined(PM_AUDIO_HAS_ONNX) && PM_AUDIO_HAS_ONNX
        if (!failed && gtcrn_open) {
            enhanced.clear();
            gtcrn.flush(enhanced);
            if (!enhanced.empty() && !write_block(enhanced.data(), enhanced.size())) {
                failed = true;
                local.reason = "couldn't write the recording";
            }
        }
#endif
        if (tap_live && tap_on.load(std::memory_order_relaxed) && file_rate != k_speech_rate) {
            tap_down.clear();
            tap_rate.flush(tap_down);
            if (!tap_down.empty())
                tap_push(tap_down.data(), tap_down.size());
        }

        const bool closed = wav.close();
        if (!closed && !failed) {
            failed = true;
            local.reason = "couldn't write the recording";
        }
        local.running = false;
        if (stop.load(std::memory_order_relaxed) && !failed) {
            local.ok = true;
            local.reason = "saved";
        } else if (failed) {
            local.ok = false;
        } else {
            local.ok = true;
            local.reason = "saved";
        }
        publish(local);
    }
};

Engine& engine()
{
    static Engine instance;
    return instance;
}

std::string join_err(std::string left, const std::string& right)
{
    if (right.empty())
        return left;
    if (!left.empty())
        left += "; ";
    left += right;
    return left;
}

std::string logic_checks_impl()
{
    std::string err = join_err(RateConv::check(), WavOut::check());
    err = join_err(err, PcmTap::check());

    Engine& eng = engine();
    eng.tap_clear();
    eng.tap_on.store(true, std::memory_order_relaxed);
    std::atomic<bool> pushing{true};
    std::thread writer([&] {
        const std::int16_t sample = 1;
        while (pushing.load(std::memory_order_relaxed))
            eng.tap_push(&sample, 1);
    });
    std::this_thread::sleep_for(std::chrono::milliseconds(2));
    eng.tap_on.store(false, std::memory_order_release);
    eng.tap_clear();
    pushing.store(false, std::memory_order_relaxed);
    writer.join();
    std::int16_t leftover = 0;
    if (eng.tap_pull(&leftover, 1) != 0)
        err = join_err(err, "tap accepted samples after it was turned off");

    if (!aaudio_available()) {
        const LeaseGrant grant = lease_acquire(SessionKind::Recorder);
        if (!grant.ok || grant.token == 0) {
            err = join_err(err, "recorder lease");
        } else {
            RecorderRequest req;
            req.token = grant.token;
            req.wav_path = (std::filesystem::temp_directory_path() / "pm-audio-recorder-idle.wav").string();
            std::atomic<int> stuck{0};
            std::vector<std::thread> calls;
            for (int i = 0; i < 4; ++i) {
                calls.emplace_back([&] {
                    const RecorderStatus started = recorder_start(req);
                    if (started.reason == "already recording")
                        stuck.fetch_add(1);
                });
                calls.emplace_back([] { recorder_stop(); });
            }
            for (auto& call : calls)
                call.join();
            recorder_stop();
            const RecorderStatus again = recorder_start(req);
            if (stuck.load() != 0 || again.reason == "already recording")
                err = join_err(err, "recorder stayed busy after a failed start");
            recorder_stop();
            lease_release(grant.token);
        }
    }
    return err;
}

} // namespace

RecorderStatus recorder_start(const RecorderRequest& request)
{
    Engine& eng = engine();
    std::lock_guard<std::mutex> gate(eng.life);
    if (!eng.worker_done.load(std::memory_order_acquire)) {
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

    eng.stop.store(false, std::memory_order_relaxed);
    eng.want_gtcrn.store(request.gtcrn, std::memory_order_relaxed);
    eng.want_aec.store(request.aec, std::memory_order_relaxed);
    eng.want_agc.store(request.agc, std::memory_order_relaxed);
    eng.tap_clear();
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
    eng.worker_done.store(false, std::memory_order_release);
    try {
        eng.worker = std::thread([rate, file_rate, path = request.wav_path, model = request.model_path,
                                  token = request.token] {
            engine().run(rate, file_rate, std::move(path), std::move(model), token);
        });
    } catch (const std::exception& ex) {
        pm::audio::log::warn(std::string("[recorder] thread: ") + ex.what());
        aaudio_capture_stop();
        eng.worker_done.store(true, std::memory_order_release);
        RecorderStatus denied;
        denied.reason = "recording thread did not start";
        eng.publish(denied);
        return denied;
    }
    return eng.copy();
}

RecorderStatus recorder_set(bool gtcrn, bool aec, bool agc)
{
    Engine& eng = engine();
    eng.want_gtcrn.store(gtcrn, std::memory_order_relaxed);
    eng.want_aec.store(aec, std::memory_order_relaxed);
    eng.want_agc.store(agc, std::memory_order_relaxed);
    RecorderStatus status = eng.copy();
    status.gtcrn_requested = gtcrn;
    status.aec_requested = aec;
    status.agc_requested = agc;
    if (!status.running && eng.worker_done.load(std::memory_order_acquire)) {
        status.ok = true;
        status.reason = "idle";
    }
    return status;
}

RecorderStatus recorder_stop()
{
    Engine& eng = engine();
    std::lock_guard<std::mutex> gate(eng.life);
    eng.stop.store(true, std::memory_order_release);
    if (eng.worker.joinable())
        eng.worker.join();
    eng.worker_done.store(true, std::memory_order_release);
    eng.stop.store(false, std::memory_order_relaxed);
    eng.tap_clear();
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
    if (!on) {
        eng.tap_on.store(false, std::memory_order_release);
        eng.tap_clear();
        return;
    }
    eng.tap_on.store(true, std::memory_order_release);
}

std::size_t recorder_pull_pcm(std::int16_t* dst, std::size_t max_samples)
{
    return engine().tap_pull(dst, max_samples);
}

std::string recorder_logic_checks()
{
    return logic_checks_impl();
}

} // namespace pm::audio::session
