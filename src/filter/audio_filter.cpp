#include "pm/audio/filter/audio_filter.hpp"


#include "pm/audio/filter/deepfilter.hpp"
#include "pm/audio/filter/gtcrn.hpp"
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
#include <cctype>
#include <cstring>
#include <fstream>
#include <vector>
#include "string/string_utils.hpp"

namespace pm::audio::filter {
namespace {


bool write_s16_wav(const std::filesystem::path& path,
                   const int16_t* samples,
                   size_t n_samples,
                   uint32_t sample_rate,
                   uint16_t channels)
{
    std::ofstream f(path, std::ios::binary | std::ios::trunc);
    if (!f) return false;

    const uint32_t data_bytes = static_cast<uint32_t>(n_samples * sizeof(int16_t));
    const uint32_t byte_rate  = sample_rate * channels * 2u;
    const uint16_t block_align = static_cast<uint16_t>(channels * 2u);
    const uint32_t riff_size  = 36u + data_bytes;

    auto w16 = [&](uint16_t v) { f.write(reinterpret_cast<const char*>(&v), 2); };
    auto w32 = [&](uint32_t v) { f.write(reinterpret_cast<const char*>(&v), 4); };

    f.write("RIFF", 4); w32(riff_size);
    f.write("WAVE", 4);
    f.write("fmt ", 4); w32(16);
    w16(1);
    w16(channels);
    w32(sample_rate);
    w32(byte_rate);
    w16(block_align);
    w16(16);
    f.write("data", 4); w32(data_bytes);
    f.write(reinterpret_cast<const char*>(samples),
            static_cast<std::streamsize>(data_bytes));
    return f.good();
}

bool decode_to_mono(const std::filesystem::path& src,
                    uint32_t sample_rate,
                    std::vector<float>& pcm,
                    std::string& err)
{
    const std::string path = media::path::path_to_utf8(src);
    ma_decoder_config cfg = ma_decoder_config_init(ma_format_f32, 1, sample_rate);
    ma_decoder decoder{};
    const ma_result init = ma_decoder_init_file(path.c_str(), &cfg, &decoder);
    if (init != MA_SUCCESS) {
        err = "cannot decode audio file";
        return false;
    }

    ma_uint64 frames = 0;
    if (ma_decoder_get_length_in_pcm_frames(&decoder, &frames) != MA_SUCCESS || frames == 0) {
        // Length unknown (some streams): read incrementally.
        std::vector<float> chunk(sample_rate);
        for (;;) {
            ma_uint64 got = 0;
            if (ma_decoder_read_pcm_frames(&decoder, chunk.data(), chunk.size(), &got) != MA_SUCCESS)
                break;
            if (got == 0) break;
            pcm.insert(pcm.end(), chunk.begin(), chunk.begin() + static_cast<size_t>(got));
        }
    } else {
        pcm.resize(static_cast<size_t>(frames));
        ma_uint64 got = 0;
        const ma_result read = ma_decoder_read_pcm_frames(
            &decoder, pcm.data(), frames, &got);
        if (read != MA_SUCCESS || got == 0) {
            ma_decoder_uninit(&decoder);
            err = "failed to read audio frames";
            return false;
        }
        pcm.resize(static_cast<size_t>(got));
    }
    ma_decoder_uninit(&decoder);
    if (pcm.empty()) {
        err = "decoded audio is empty";
        return false;
    }
    return true;
}

bool decode_to_48k_mono(const std::filesystem::path& src, std::vector<float>& pcm, std::string& err)
{
    return decode_to_mono(src, 48000, pcm, err);
}

} // namespace

Kind parse_kind(std::string_view token)
{
    std::string s(token);
    s = media::strutil::lower_ascii(std::move(s));
    if (s.empty() || s == "deepfilter" || s == "dfn" || s == "deepfilternet"
        || s == "deepfilternet3")
        return Kind::DeepFilter;
    if (s == "gtcrn")
        return Kind::Gtcrn;
    return Kind::DeepFilter;
}

const char* kind_name(Kind kind) noexcept
{
    switch (kind) {
    case Kind::Gtcrn:      return "gtcrn";
    case Kind::DeepFilter:
    default:               return "deepfilter";
    }
}

bool available(Kind kind)
{
    switch (kind) {
    case Kind::DeepFilter: return dfn::available();
    case Kind::Gtcrn:      return gtcrn::available();
    }
    return false;
}

std::string available_backend(Kind kind)
{
    switch (kind) {
    case Kind::DeepFilter: return dfn::backend_name();
    case Kind::Gtcrn:      return gtcrn::backend_name();
    }
    return {};
}

Result decode_to_wav(const std::filesystem::path& src,
                     const std::filesystem::path& dst,
                     uint32_t sample_rate)
{
    Result r;
    r.kind = "decode";
    if (sample_rate < 8000) {
        r.error = "sample rate must be at least 8 kHz";
        return r;
    }

    std::error_code ec;
    if (src.empty() || !std::filesystem::is_regular_file(src, ec) || ec) {
        r.error = "source file not found";
        return r;
    }
    if (dst.empty()) {
        r.error = "destination path is empty";
        return r;
    }

    std::vector<float> pcm;
    std::string dec_err;
    if (!decode_to_mono(src, sample_rate, pcm, dec_err)) {
        r.error = dec_err;
        return r;
    }

    std::vector<int16_t> s16(pcm.size());
    for (size_t i = 0; i < pcm.size(); ++i) {
        const float x = std::clamp(pcm[i], -1.0f, 1.0f);
        s16[i] = static_cast<int16_t>(x * 32767.0f);
    }

    const auto parent = dst.parent_path();
    if (!parent.empty()) {
        std::error_code dir_ec;
        std::filesystem::create_directories(parent, dir_ec);
        if (dir_ec) {
            r.error = "cannot create output directory";
            return r;
        }
    }
    if (!write_s16_wav(dst, s16.data(), s16.size(), sample_rate, 1)) {
        r.error = "failed to write destination WAV";
        return r;
    }

    r.ok = true;
    r.sample_rate = sample_rate;
    r.channels = 1;
    r.frames = static_cast<uint64_t>(s16.size());
    return r;
}

Result apply_file(const std::filesystem::path& src,
                  const std::filesystem::path& dst,
                  const Options& opt)
{
    Result r;
    r.kind = kind_name(opt.kind);

    std::error_code ec;
    if (src.empty() || !std::filesystem::is_regular_file(src, ec) || ec) {
        r.error = "source file not found";
        return r;
    }
    if (dst.empty()) {
        r.error = "destination path is empty";
        return r;
    }
    ec.clear();
    if (std::filesystem::exists(dst, ec) && !ec) {
        std::error_code eq_ec;
        if (std::filesystem::equivalent(src, dst, eq_ec) && !eq_ec) {
            r.error = "source and destination must be different files";
            return r;
        }
    }

    if (opt.kind == Kind::Gtcrn)
        return gtcrn::apply_file(src, dst, opt);

    if (!dfn::available()) {
        r.error = "DeepFilterNet DLL not loaded. Stage deepfilter.dll next to the exe "
                  "(packages/DeepFilterNet/build-capi.ps1).";
        return r;
    }

    std::vector<float> pcm;
    std::string dec_err;
    if (!decode_to_48k_mono(src, pcm, dec_err)) {
        r.error = dec_err;
        return r;
    }

    r = dfn::enhance_48k_mono(pcm, opt, src);
    if (!r.ok)
        return r;

    std::vector<int16_t> s16(pcm.size());
    for (size_t i = 0; i < pcm.size(); ++i) {
        const float x = std::clamp(pcm[i], -1.0f, 1.0f);
        s16[i] = static_cast<int16_t>(x * 32767.0f);
    }

    const auto parent = dst.parent_path();
    if (!parent.empty()) {
        std::error_code dir_ec;
        std::filesystem::create_directories(parent, dir_ec);
        if (dir_ec) {
            r.ok = false;
            r.error = "cannot create output directory";
            return r;
        }
    }

    if (!write_s16_wav(dst, s16.data(), s16.size(), 48000, 1)) {
        r.ok = false;
        r.error = "failed to write destination WAV";
        return r;
    }

    r.ok = true;
    r.sample_rate = 48000;
    r.channels = 1;
    r.frames = static_cast<uint64_t>(s16.size());
    r.kind = kind_name(opt.kind);
    pm::audio::log::info("[audio-filter] wrote " + media::path::path_to_utf8(dst)
                 + " backend=" + r.backend
                 + " frames=" + std::to_string(r.frames));
    return r;
}

Result apply_file_inplace(const std::filesystem::path& path, const Options& opt)
{
    Result r;
    r.kind = kind_name(opt.kind);
    if (path.empty()) {
        r.error = "destination path is empty";
        return r;
    }

    auto tmp = path;
    tmp += ".enhancing.wav";
    r = apply_file(path, tmp, opt);
    if (!r.ok) {
        std::error_code rm_ec;
        std::filesystem::remove(tmp, rm_ec);
        return r;
    }

    std::error_code rm_ec;
    std::filesystem::remove(path, rm_ec);
    std::error_code mv_ec;
    std::filesystem::rename(tmp, path, mv_ec);
    if (mv_ec) {
        std::error_code cp_ec;
        std::filesystem::copy_file(
            tmp, path, std::filesystem::copy_options::overwrite_existing, cp_ec);
        std::filesystem::remove(tmp, rm_ec);
        if (cp_ec) {
            r.ok = false;
            r.error = "failed to replace source with filtered file";
            return r;
        }
    }
    return r;
}

std::int64_t duration_ms(const std::filesystem::path& src)
{
    if (src.empty())
        return 0;
    const std::string path = media::path::path_to_utf8(src);
    ma_decoder decoder{};
    if (ma_decoder_init_file(path.c_str(), nullptr, &decoder) != MA_SUCCESS)
        return 0;
    ma_uint64 frames = 0;
    ma_decoder_get_length_in_pcm_frames(&decoder, &frames);
    const ma_uint32 rate = decoder.outputSampleRate;
    ma_decoder_uninit(&decoder);
    if (rate == 0 || frames == 0)
        return 0;
    return static_cast<std::int64_t>(frames * 1000ull / rate);
}

} // namespace pm::audio::filter

