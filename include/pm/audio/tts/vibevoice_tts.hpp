#pragma once

#include <chrono>
#include <mutex>
#include <string>
#include <string_view>
#include <vector>

#include <nlohmann/json.hpp>

namespace pm::tts {

struct VibevoiceBackend {
    int (*load)(const char* model, const char* tokenizer, const char* voice, int n_threads) = nullptr;
    void (*unload)() = nullptr;
    int (*tts)(const char* text, const char* voice, const char* ref_audio,
               const char* out_wav, int steps, float cfg, int max_frames, unsigned seed) = nullptr;
};

void set_vibevoice_backend(const VibevoiceBackend& backend);

struct VibevoiceSynthResult {
    std::vector<float> samples;
    int sample_rate = 24000;
    long long synth_ms = 0;
    double rtf() const {
        if (samples.empty() || sample_rate <= 0)
            return 0.0;
        const double audio_ms = static_cast<double>(samples.size()) /
                                static_cast<double>(sample_rate) * 1000.0;
        return audio_ms > 0.0 ? static_cast<double>(synth_ms) / audio_ms : 0.0;
    }
};

class VibevoiceEngine {
public:
    static VibevoiceEngine& global();

    int ensure(const std::string& model, const std::string& tokenizer,
               const std::string& voice, int n_threads = 0);

    int tts(const std::string& text, const std::string& voice,
            const std::string& ref_audio, const std::string& out_wav,
            int steps = 0, float cfg = 0.0f, int max_frames = 0,
            unsigned seed = 0);

    void unload();

    bool is_loaded() const;
    nlohmann::json loaded_models_json() const;
    bool unload_loaded_model(std::string_view key, std::string& err);

    ~VibevoiceEngine();

private:
    VibevoiceEngine() = default;
    mutable std::mutex mu_;
    std::string model_, tokenizer_, voice_;
    bool loaded_ = false;
};

int vibevoice_tts_synthesize_pcm(
    const std::string& text,
    const std::string& model_path,
    const std::string& tokenizer_path,
    const std::string& voice_path,
    VibevoiceSynthResult& out,
    const std::string& ref_audio_path = {},
    int n_threads = 0,
    int steps = 0,
    float cfg = 0.0f,
    int max_frames = 0,
    unsigned seed = 0);

int vibevoice_tts_synthesize(
    const std::string& text,
    const std::string& model_path,
    const std::string& tokenizer_path,
    const std::string& voice_path,
    const std::string& dst_wav,
    const std::string& ref_audio_path = {},
    int n_threads = 0,
    int steps = 0,
    float cfg = 0.0f,
    int max_frames = 0,
    unsigned seed = 0);

} // namespace pm::tts
