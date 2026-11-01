#pragma once

#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

namespace pm::stt {

struct WavPcm {
    std::vector<float> mono;
    std::uint32_t sample_rate = 0;
};

// PCM16 or float32, any channel count. Throws std::runtime_error.
WavPcm read_wav(const std::string& path);

// Linear resample to 16 kHz mono. Empty input stays empty.
std::vector<float> resample_16k(const WavPcm& wav);

// read_wav then resample_16k. Throws std::runtime_error.
std::vector<float> read_wav_16k(const std::string& path);

// 16-bit mono PCM. Returns false when the file cannot be written.
bool write_pcm_wav(const std::string& path,
                   const short* samples,
                   std::size_t n_samples,
                   int sample_rate = 16000);

} // namespace pm::stt
