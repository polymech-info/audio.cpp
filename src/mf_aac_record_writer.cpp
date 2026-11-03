#include "pm/audio/mf_aac_record.hpp"

#if defined(_WIN32)

#include "pm/audio/constants.hpp"
#include "pm/audio/path.hpp"
#include "fs/path_sanitizer.hpp"

#ifndef NOMINMAX
#  define NOMINMAX
#endif
#ifndef WIN32_LEAN_AND_MEAN
#  define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#include <stringapiset.h>
#include <mfapi.h>
#include <mfidl.h>
#include <mfreadwrite.h>
#include <mferror.h>

#include <algorithm>
#include <atomic>
#include <cstring>
#include <mutex>
#include <vector>

#if defined(_MSC_VER)
#  pragma warning(push, 0)
#endif
#include "miniaudio.h"
#if defined(_MSC_VER)
#  pragma warning(pop)
#endif

#pragma comment(lib, "mfplat.lib")
#pragma comment(lib, "mfreadwrite.lib")
#pragma comment(lib, "mfuuid.lib")

namespace pm::audio {
namespace {

namespace C = pm::audio::constants;

constexpr UINT32 kMuxSampleRate  = 48000;
constexpr UINT32 kAacBytesPerSec = 24000;
constexpr LONGLONG kHnsPerMuxSample =
    10'000'000LL / static_cast<LONGLONG>(kMuxSampleRate);

std::atomic<int> g_mf_users{0};

void mf_retain() noexcept
{
    if (g_mf_users.fetch_add(1) + 1 == 1)
        (void)MFStartup(MF_VERSION, MFSTARTUP_FULL);
}

void mf_release() noexcept
{
    if (g_mf_users.fetch_sub(1) - 1 == 0)
        (void)MFShutdown();
}

std::wstring widen(const std::filesystem::path& path)
{
    const std::string utf8 = media::path::path_to_utf8(path);
    if (utf8.empty()) return {};
    const int n = ::MultiByteToWideChar(CP_UTF8, 0, utf8.data(), (int)utf8.size(), nullptr, 0);
    if (n <= 0) return std::wstring(utf8.begin(), utf8.end());
    std::wstring w((size_t)n, L'\0');
    ::MultiByteToWideChar(CP_UTF8, 0, utf8.data(), (int)utf8.size(), w.data(), n);
    return w;
}

template <class T>
void safe_release(T*& p) noexcept
{
    if (p) { p->Release(); p = nullptr; }
}

} // namespace

struct MfAacRecordWriter::Impl {
    std::mutex write_mu;
    IMFSinkWriter* sink   = nullptr;
    DWORD          stream = 0;
    bool           mf     = false;

    uint32_t capture_rate     = C::k_recording_sample_rate;
    uint16_t capture_channels = static_cast<uint16_t>(C::k_recording_channels);
    uint16_t mux_channels     = 2;
    uint64_t frames_written   = 0;

    LONGLONG last_pts = 0;

    ma_resampler         resampler{};
    bool                 resampler_open = false;
    std::vector<int16_t> resample_out;

    void init_resampler()
    {
        if (resampler_open) return;
        if (capture_rate == kMuxSampleRate && capture_channels == mux_channels)
            return;
        const ma_resampler_config rc = ma_resampler_config_init(
            ma_format_s16,
            mux_channels,
            capture_rate,
            kMuxSampleRate,
            ma_resample_algorithm_linear);
        resampler_open = (ma_resampler_init(&rc, nullptr, &resampler) == MA_SUCCESS);
    }

    void uninit_resampler() noexcept
    {
        if (!resampler_open) return;
        ma_resampler_uninit(&resampler, nullptr);
        resampler_open = false;
    }

    bool write_pcm_locked(const int16_t* pcm, size_t frames)
    {
        if (!sink || frames == 0 || !pcm) return true;
        const DWORD nbytes =
            static_cast<DWORD>(frames * mux_channels * sizeof(int16_t));

        IMFMediaBuffer* buf = nullptr;
        if (FAILED(MFCreateMemoryBuffer(nbytes, &buf))) return false;
        BYTE* dst = nullptr;
        DWORD maxlen = 0, curlen = 0;
        if (FAILED(buf->Lock(&dst, &maxlen, &curlen))) {
            buf->Release();
            return false;
        }
        std::memcpy(dst, pcm, nbytes);
        buf->Unlock();
        buf->SetCurrentLength(nbytes);

        IMFSample* smp = nullptr;
        if (FAILED(MFCreateSample(&smp))) {
            buf->Release();
            return false;
        }
        smp->AddBuffer(buf);

        const LONGLONG pts = last_pts;
        const LONGLONG dur = static_cast<LONGLONG>(frames) * kHnsPerMuxSample;
        smp->SetSampleTime(pts);
        smp->SetSampleDuration(std::max<LONGLONG>(dur, 1));
        last_pts = pts + dur;

        const HRESULT hr = sink->WriteSample(stream, smp);
        smp->Release();
        buf->Release();
        if (!SUCCEEDED(hr)) return false;

        frames_written += frames;
        return true;
    }

    bool write_capture_pcm(const int16_t* pcm, size_t frames)
    {
        if (!pcm || frames == 0) return true;

        if (capture_rate == kMuxSampleRate && capture_channels == mux_channels)
            return write_pcm_locked(pcm, frames);

        init_resampler();
        if (!resampler_open)
            return write_pcm_locked(pcm, frames);

        ma_uint64 in_frames  = frames;
        const ma_uint64 out_cap =
            static_cast<ma_uint64>(frames) * kMuxSampleRate / capture_rate + 64;
        resample_out.resize(static_cast<size_t>(out_cap * mux_channels));
        ma_uint64 out_frames = out_cap;
        const ma_result rr = ma_resampler_process_pcm_frames(
            &resampler, pcm, &in_frames, resample_out.data(), &out_frames);
        if (rr != MA_SUCCESS || out_frames == 0)
            return false;
        return write_pcm_locked(resample_out.data(), static_cast<size_t>(out_frames));
    }

    void reset() noexcept
    {
        uninit_resampler();
        safe_release(sink);
        stream = 0;
        last_pts = 0;
        frames_written = 0;
        if (mf) {
            mf_release();
            mf = false;
        }
    }
};

MfAacRecordWriter::MfAacRecordWriter() : m_(std::make_unique<Impl>()) {}
MfAacRecordWriter::~MfAacRecordWriter() { std::string ignore; (void)close(ignore); }

bool MfAacRecordWriter::open(const std::filesystem::path& dst,
                             uint32_t sample_rate,
                             uint16_t channels,
                             std::string& err)
{
    err.clear();
    std::string close_err;
    (void)close(close_err);

    if (sample_rate < 8000) sample_rate = C::k_recording_sample_rate;
    if (channels == 0) channels = static_cast<uint16_t>(C::k_recording_channels);
    if (channels > 2) channels = 2;

    m_->capture_rate     = sample_rate;
    m_->capture_channels = channels;
    m_->mux_channels     = channels;

    if (!m_->mf) {
        mf_retain();
        m_->mf = true;
    }

    const std::wstring wpath = widen(dst);
    HRESULT hr = MFCreateSinkWriterFromURL(wpath.c_str(), nullptr, nullptr, &m_->sink);
    if (FAILED(hr)) {
        err = "aac record: MFCreateSinkWriterFromURL failed (use .m4a or .aac extension)";
        m_->reset();
        return false;
    }

    const UINT32 pcm_bytes_per_sec =
        kMuxSampleRate * m_->mux_channels * sizeof(int16_t);

    IMFMediaType* outT = nullptr;
    hr = MFCreateMediaType(&outT);
    if (SUCCEEDED(hr)) {
        outT->SetGUID(MF_MT_MAJOR_TYPE, MFMediaType_Audio);
        outT->SetGUID(MF_MT_SUBTYPE, MFAudioFormat_AAC);
        outT->SetUINT32(MF_MT_AUDIO_NUM_CHANNELS, m_->mux_channels);
        outT->SetUINT32(MF_MT_AUDIO_SAMPLES_PER_SECOND, kMuxSampleRate);
        outT->SetUINT32(MF_MT_AUDIO_BITS_PER_SAMPLE, 16);
        outT->SetUINT32(MF_MT_AUDIO_AVG_BYTES_PER_SECOND, kAacBytesPerSec);
        outT->SetUINT32(MF_MT_AAC_PAYLOAD_TYPE, 0);
        outT->SetUINT32(MF_MT_AAC_AUDIO_PROFILE_LEVEL_INDICATION, 0x29);
        hr = m_->sink->AddStream(outT, &m_->stream);
        outT->Release();
    }
    if (FAILED(hr)) {
        err = "aac record: AddStream AAC failed";
        m_->reset();
        return false;
    }

    IMFMediaType* inT = nullptr;
    hr = MFCreateMediaType(&inT);
    if (SUCCEEDED(hr)) {
        inT->SetGUID(MF_MT_MAJOR_TYPE, MFMediaType_Audio);
        inT->SetGUID(MF_MT_SUBTYPE, MFAudioFormat_PCM);
        inT->SetUINT32(MF_MT_AUDIO_NUM_CHANNELS, m_->mux_channels);
        inT->SetUINT32(MF_MT_AUDIO_SAMPLES_PER_SECOND, kMuxSampleRate);
        inT->SetUINT32(MF_MT_AUDIO_BITS_PER_SAMPLE, 16);
        inT->SetUINT32(MF_MT_AUDIO_BLOCK_ALIGNMENT,
                        m_->mux_channels * sizeof(int16_t));
        inT->SetUINT32(MF_MT_AUDIO_AVG_BYTES_PER_SECOND, pcm_bytes_per_sec);
        hr = m_->sink->SetInputMediaType(m_->stream, inT, nullptr);
        inT->Release();
    }
    if (FAILED(hr)) {
        err = "aac record: SetInputMediaType PCM failed";
        m_->reset();
        return false;
    }

    hr = m_->sink->BeginWriting();
    if (FAILED(hr)) {
        err = "aac record: BeginWriting failed";
        m_->reset();
        return false;
    }

    return true;
}

void MfAacRecordWriter::write_pcm(const int16_t* pcm, size_t frame_count)
{
    if (!pcm || frame_count == 0 || !m_->sink) return;
    std::lock_guard<std::mutex> lk(m_->write_mu);
    (void)m_->write_capture_pcm(pcm, frame_count);
}

bool MfAacRecordWriter::close(std::string& err)
{
    err.clear();
    std::lock_guard<std::mutex> lk(m_->write_mu);
    if (!m_->sink) return true;

    const HRESULT hr = m_->sink->Finalize();
    m_->reset();
    if (FAILED(hr)) {
        err = "aac record: Finalize failed";
        return false;
    }
    return true;
}

bool MfAacRecordWriter::is_open() const noexcept
{
    return m_->sink != nullptr;
}

uint32_t MfAacRecordWriter::frame_count() const noexcept
{
    std::lock_guard<std::mutex> lk(m_->write_mu);
    return static_cast<uint32_t>(m_->frames_written);
}

uint32_t MfAacRecordWriter::sample_rate() const noexcept
{
    return kMuxSampleRate;
}

uint16_t MfAacRecordWriter::channels() const noexcept
{
    return m_->mux_channels;
}

} // namespace pm::audio

#endif
