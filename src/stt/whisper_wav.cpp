#include "pm/audio/stt/wav.hpp"
#include "pm/audio/text.hpp"

#include <algorithm>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <stdexcept>
#include <string>

namespace pm::stt {
namespace {

std::uint32_t rd_u32(const std::uint8_t* p)
{
    return p[0] | (p[1] << 8) | (p[2] << 16) | (std::uint32_t(p[3]) << 24);
}

std::uint16_t rd_u16(const std::uint8_t* p)
{
    return static_cast<std::uint16_t>(p[0] | (p[1] << 8));
}

#pragma pack(push, 1)
struct WavHdr {
    char riff[4] = {'R', 'I', 'F', 'F'};
    std::uint32_t chunk_size = 0;
    char wave[4] = {'W', 'A', 'V', 'E'};
    char fmt_id[4] = {'f', 'm', 't', ' '};
    std::uint32_t fmt_size = 16;
    std::uint16_t audio_format = 1;
    std::uint16_t channels = 1;
    std::uint32_t sample_rate = 16000;
    std::uint32_t byte_rate = 32000;
    std::uint16_t block_align = 2;
    std::uint16_t bits_per_sample = 16;
    char data_id[4] = {'d', 'a', 't', 'a'};
    std::uint32_t data_size = 0;
};
#pragma pack(pop)
static_assert(sizeof(WavHdr) == 44, "WavHdr must be 44 bytes");

} // namespace

WavPcm read_wav(const std::string& path)
{
    std::ifstream f(pm::audio::path_from_utf8(path), std::ios::binary);
    if (!f)
        throw std::runtime_error("whisper_local: cannot open WAV: " + path);
    std::vector<std::uint8_t> buf((std::istreambuf_iterator<char>(f)), std::istreambuf_iterator<char>());
    if (buf.size() < 44 || std::memcmp(buf.data(), "RIFF", 4) != 0 || std::memcmp(buf.data() + 8, "WAVE", 4) != 0)
        throw std::runtime_error("whisper_local: not a RIFF/WAVE file: " + path);

    std::uint16_t fmt = 0, channels = 0, bits = 0;
    std::uint32_t rate = 0;
    const std::uint8_t* data_ptr = nullptr;
    std::uint32_t data_len = 0;

    std::size_t pos = 12;
    while (pos + 8 <= buf.size()) {
        const std::uint8_t* ck = buf.data() + pos;
        const std::uint32_t ck_size = rd_u32(ck + 4);
        const std::uint8_t* ck_body = ck + 8;
        if (std::memcmp(ck, "fmt ", 4) == 0 && ck_size >= 16) {
            fmt = rd_u16(ck_body + 0);
            channels = rd_u16(ck_body + 2);
            rate = rd_u32(ck_body + 4);
            bits = rd_u16(ck_body + 14);
        } else if (std::memcmp(ck, "data", 4) == 0) {
            data_ptr = ck_body;
            data_len = std::min<std::uint32_t>(ck_size, static_cast<std::uint32_t>(buf.size() - (pos + 8)));
        }
        pos += 8 + ck_size + (ck_size & 1u);
    }

    if (!data_ptr || channels == 0 || rate == 0)
        throw std::runtime_error("whisper_local: malformed WAV (missing fmt/data): " + path);
    const bool is_float = (fmt == 3) || (fmt == 0xFFFE && bits == 32);

    WavPcm out;
    out.sample_rate = rate;

    if (is_float && bits == 32) {
        const std::size_t n = data_len / 4;
        const std::size_t frames = n / channels;
        out.mono.resize(frames);
        const float* s = reinterpret_cast<const float*>(data_ptr);
        for (std::size_t i = 0; i < frames; ++i) {
            float acc = 0.f;
            for (std::uint16_t c = 0; c < channels; ++c)
                acc += s[i * channels + c];
            out.mono[i] = acc / channels;
        }
    } else if (bits == 16) {
        const std::size_t n = data_len / 2;
        const std::size_t frames = n / channels;
        out.mono.resize(frames);
        const std::int16_t* s = reinterpret_cast<const std::int16_t*>(data_ptr);
        for (std::size_t i = 0; i < frames; ++i) {
            int acc = 0;
            for (std::uint16_t c = 0; c < channels; ++c)
                acc += s[i * channels + c];
            out.mono[i] = static_cast<float>(acc) / channels / 32768.0f;
        }
    } else {
        throw std::runtime_error("whisper_local: unsupported WAV sample format (fmt=" +
                                 std::to_string(fmt) + ", bits=" + std::to_string(bits) + ")");
    }
    return out;
}

std::vector<float> resample_16k(const WavPcm& wav)
{
    constexpr std::uint32_t kTarget = 16000;
    if (wav.sample_rate == kTarget || wav.mono.empty())
        return wav.mono;
    const double ratio = static_cast<double>(kTarget) / static_cast<double>(wav.sample_rate);
    const std::size_t out_n = static_cast<std::size_t>(wav.mono.size() * ratio);
    std::vector<float> out(out_n);
    for (std::size_t i = 0; i < out_n; ++i) {
        const double src = i / ratio;
        const std::size_t i0 = static_cast<std::size_t>(src);
        const std::size_t i1 = std::min(i0 + 1, wav.mono.size() - 1);
        const float fr = static_cast<float>(src - i0);
        out[i] = wav.mono[i0] * (1.f - fr) + wav.mono[i1] * fr;
    }
    return out;
}

std::vector<float> read_wav_16k(const std::string& path)
{
    return resample_16k(read_wav(path));
}

bool write_pcm_wav(const std::string& path,
                   const short* samples,
                   std::size_t n_samples,
                   int sample_rate)
{
    std::ofstream f(pm::audio::path_from_utf8(path), std::ios::binary | std::ios::trunc);
    if (!f)
        return false;
    const std::uint32_t data_bytes = static_cast<std::uint32_t>(n_samples * sizeof(short));
    WavHdr hdr;
    hdr.sample_rate = static_cast<std::uint32_t>(sample_rate);
    hdr.byte_rate = static_cast<std::uint32_t>(sample_rate) * 2u;
    hdr.data_size = data_bytes;
    hdr.chunk_size = data_bytes + 36;
    f.write(reinterpret_cast<const char*>(&hdr), sizeof(hdr));
    if (samples && data_bytes)
        f.write(reinterpret_cast<const char*>(samples), static_cast<std::streamsize>(data_bytes));
    return f.good();
}

} // namespace pm::stt
