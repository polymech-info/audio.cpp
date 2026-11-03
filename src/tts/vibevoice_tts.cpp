#include "pm/audio/tts/vibevoice_tts.hpp"
#include "fs/path_sanitizer.hpp"
#if defined(PM_AUDIO_LINK_VIBEVOICE) && PM_AUDIO_LINK_VIBEVOICE
#include <vibevoice_capi.h>
#endif


#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <string>
#include <vector>

namespace pm::tts {

namespace {

// Minimal WAV reader: reads RIFF/PCM s16le, converts to float32.
bool read_wav_pcm(const std::string& path, std::vector<float>& out, int& sample_rate)
{
    std::ifstream f(path, std::ios::binary);
    if (!f) return false;

    char riff[4]; f.read(riff, 4);
    if (std::memcmp(riff, "RIFF", 4) != 0) return false;

    uint32_t chunk_size = 0; f.read(reinterpret_cast<char*>(&chunk_size), 4);
    char wave[4]; f.read(wave, 4);
    if (std::memcmp(wave, "WAVE", 4) != 0) return false;

    uint16_t audio_format = 0, channels = 0, bits = 0;
    uint32_t sr = 0, data_bytes = 0;
    bool got_fmt = false, got_data = false;

    while (f && !got_data) {
        char id[4] = {}; f.read(id, 4);
        uint32_t sz = 0; f.read(reinterpret_cast<char*>(&sz), 4);
        if (!f) break;

        if (std::memcmp(id, "fmt ", 4) == 0) {
            f.read(reinterpret_cast<char*>(&audio_format), 2);
            f.read(reinterpret_cast<char*>(&channels),     2);
            f.read(reinterpret_cast<char*>(&sr),           4);
            uint32_t br = 0; f.read(reinterpret_cast<char*>(&br), 4);
            uint16_t ba = 0; f.read(reinterpret_cast<char*>(&ba), 2);
            f.read(reinterpret_cast<char*>(&bits),         2);
            if (sz > 16) f.seekg(sz - 16, std::ios::cur);
            got_fmt = true;
        } else if (std::memcmp(id, "data", 4) == 0) {
            data_bytes = sz;
            got_data = got_fmt;
            if (!got_fmt) f.seekg(sz, std::ios::cur);
        } else {
            f.seekg(sz, std::ios::cur);
        }
    }

    if (!got_fmt || !got_data || audio_format != 1 || bits != 16) return false;

    sample_rate = static_cast<int>(sr);
    const size_t n_samples = data_bytes / 2;
    out.resize(n_samples);
    for (size_t i = 0; i < n_samples; ++i) {
        int16_t v = 0; f.read(reinterpret_cast<char*>(&v), 2);
        out[i] = static_cast<float>(v) / 32768.0f;
    }
    return true;
}

VibevoiceBackend& vibevoice_backend()
{
    static VibevoiceBackend backend;
    return backend;
}

int vv_load(const char* model, const char* tokenizer, const char* voice, int n_threads)
{
    if (vibevoice_backend().load)
        return vibevoice_backend().load(model, tokenizer, voice, n_threads);
#if defined(PM_AUDIO_LINK_VIBEVOICE) && PM_AUDIO_LINK_VIBEVOICE
    return vv_capi_load(model, nullptr, tokenizer, voice, n_threads);
#else
    (void)model; (void)tokenizer; (void)voice; (void)n_threads;
    return -100;
#endif
}

void vv_unload()
{
    if (vibevoice_backend().unload) {
        vibevoice_backend().unload();
        return;
    }
#if defined(PM_AUDIO_LINK_VIBEVOICE) && PM_AUDIO_LINK_VIBEVOICE
    vv_capi_unload();
#endif
}

int vv_tts(const char* text, const char* voice, const char* ref_audio,
           const char* out_wav, int steps, float cfg, int max_frames, unsigned seed)
{
    if (vibevoice_backend().tts)
        return vibevoice_backend().tts(text, voice, ref_audio, out_wav, steps, cfg, max_frames, seed);
#if defined(PM_AUDIO_LINK_VIBEVOICE) && PM_AUDIO_LINK_VIBEVOICE
    const char* refs[] = { ref_audio };
    return vv_capi_tts(text, voice,
                       ref_audio ? refs : nullptr, ref_audio ? 1 : 0,
                       out_wav, steps, cfg, max_frames, static_cast<uint32_t>(seed));
#else
    (void)text; (void)voice; (void)ref_audio; (void)out_wav;
    (void)steps; (void)cfg; (void)max_frames; (void)seed;
    return -100;
#endif
}

} // namespace

void set_vibevoice_backend(const VibevoiceBackend& backend)
{
    vibevoice_backend() = backend;
}

// ── VibevoiceEngine ───────────────────────────────────────────────────────────

VibevoiceEngine& VibevoiceEngine::global()
{
    static VibevoiceEngine s;
    // Register an atexit handler the first time this function is called.
    // atexit handlers registered AFTER a static object is constructed run
    // BEFORE that object's destructor (C++ [basic.start.term] LIFO rule).
    // This fires while the ggml/CUDA backend is still alive, so
    // vv_capi_unload() can free CUDA buffers cleanly — preventing the
    // "CUDA error: initialization error in ~ggml_backend_cuda_buffer_context"
    // crash we get when those buffers are freed after CUDA teardown.
    static const int atexit_once = (std::atexit([]() {
        auto& eng = VibevoiceEngine::global();
        std::lock_guard<std::mutex> lk(eng.mu_);
        if (eng.loaded_) {
            fprintf(stderr, "[vv] atexit: unloading engine before CUDA teardown\n");
            vv_unload();
            eng.loaded_ = false;
        }
    }), 0);
    (void)atexit_once;
    return s;
}

VibevoiceEngine::~VibevoiceEngine()
{
    // atexit handler above already called vv_capi_unload() before this runs.
    // Do not call it again here — CUDA is already torn down at this point.
    loaded_ = false;
}

bool VibevoiceEngine::is_loaded() const
{
    std::lock_guard<std::mutex> lk(mu_);
    return loaded_;
}

nlohmann::json VibevoiceEngine::loaded_models_json() const
{
    std::unique_lock<std::mutex> lk(mu_, std::try_to_lock);
    if (!lk.owns_lock()) {
        return nlohmann::json::array({
            {
                {"key", "vibevoice:tts"},
                {"kind", "vibevoice-tts"},
                {"name", "VibeVoice"},
                {"status", "busy"},
                {"provider", "vibevoice"},
                {"stopSupported", true},
            }
        });
    }
    if (!loaded_)
        return nlohmann::json::array();
    return nlohmann::json::array({
        {
            {"key", "vibevoice:tts"},
            {"kind", "vibevoice-tts"},
            {"name", media::path::path_to_utf8(media::path::path_from_utf8(model_).filename())},
            {"path", model_},
            {"tokenizerPath", tokenizer_},
            {"voicePath", voice_},
            {"status", "loaded"},
            {"provider", "vibevoice"},
            {"stopSupported", true},
        }
    });
}

void VibevoiceEngine::unload()
{
    std::lock_guard<std::mutex> lk(mu_);
    if (loaded_) { vv_unload(); loaded_ = false; }
}

bool VibevoiceEngine::unload_loaded_model(std::string_view key, std::string& err)
{
    if (!key.empty() && key != "vibevoice:tts") {
        err = "not a vibevoice model key";
        return false;
    }
    std::unique_lock<std::mutex> lk(mu_, std::try_to_lock);
    if (!lk.owns_lock()) {
        err = "vibevoice engine is busy";
        return false;
    }
    if (!loaded_) {
        err = "no vibevoice engine loaded";
        return false;
    }
    vv_unload();
    loaded_ = false;
    model_.clear();
    tokenizer_.clear();
    voice_.clear();
    return true;
}

int VibevoiceEngine::ensure(const std::string& model, const std::string& tokenizer,
                             const std::string& voice, int n_threads)
{
    std::lock_guard<std::mutex> lk(mu_);

    if (loaded_ && model_ == model && tokenizer_ == tokenizer && voice_ == voice)
        return 0;

    if (loaded_) { vv_unload(); loaded_ = false; }
    const char* tok = tokenizer.empty() ? nullptr : tokenizer.c_str();
    const char* voi = voice.empty()     ? nullptr : voice.c_str();
    const int rc = vv_load(model.c_str(), tok, voi, n_threads);
    if (rc == 0) { model_ = model; tokenizer_ = tokenizer; voice_ = voice; loaded_ = true; }
    return rc;
}

int VibevoiceEngine::tts(const std::string& text, const std::string& voice,
                          const std::string& ref_audio, const std::string& out_wav,
                          int steps, float cfg, int max_frames, unsigned seed)
{
    std::lock_guard<std::mutex> lk(mu_);
    if (!loaded_) return -1;
    const char* voi = voice.empty()     ? nullptr : voice.c_str();
    const char* ref = ref_audio.empty() ? nullptr : ref_audio.c_str();
    return vv_tts(text.c_str(), voi, ref, out_wav.c_str(),
                  steps, cfg, max_frames, seed);
}

// ── Per-call helpers (CLI path — load + synthesize + unload per invocation) ───
// These go through VibevoiceEngine::global() so they participate in the same
// singleton and cannot conflict with a concurrently warm xblox or agent engine.

int vibevoice_tts_synthesize_pcm(
    const std::string&    text,
    const std::string&    model_path,
    const std::string&    tokenizer_path,
    const std::string&    voice_path,
    VibevoiceSynthResult& result,
    const std::string&    ref_audio_path,
    int   n_threads,
    int   steps,
    float cfg,
    int   max_frames,
    unsigned seed)
{
    result = {};
    auto& eng = VibevoiceEngine::global();

    int rc = eng.ensure(model_path, tokenizer_path, voice_path, n_threads);
    if (rc != 0) return rc;

    namespace fs = std::filesystem;
    const std::string tmp = media::path::path_to_utf8((fs::temp_directory_path() / "vv_tts_tmp.wav"));

    const auto t0 = std::chrono::steady_clock::now();
    rc = eng.tts(text, voice_path, ref_audio_path, tmp, steps, cfg, max_frames, seed);
    const auto t1 = std::chrono::steady_clock::now();

    // Intentionally NOT unloading here: the CLI often calls vibevoice_tts_synthesize
    // immediately after to save a WAV, and we want to reuse the warm engine.
    // The engine is unloaded when ensure() is called with different args or at process exit.

    if (rc != 0) return rc;

    result.synth_ms = std::chrono::duration_cast<std::chrono::milliseconds>(t1 - t0).count();
    if (!read_wav_pcm(tmp, result.samples, result.sample_rate))
        return -20;

    fs::remove(tmp);
    return 0;
}

int vibevoice_tts_synthesize(
    const std::string& text,
    const std::string& model_path,
    const std::string& tokenizer_path,
    const std::string& voice_path,
    const std::string& dst_wav,
    const std::string& ref_audio_path,
    int   n_threads,
    int   steps,
    float cfg,
    int   max_frames,
    unsigned seed)
{
    auto& eng = VibevoiceEngine::global();
    int rc = eng.ensure(model_path, tokenizer_path, voice_path, n_threads);
    if (rc != 0) return rc;

    return eng.tts(text, voice_path, ref_audio_path, dst_wav, steps, cfg, max_frames, seed);
    // Engine stays warm — unloaded by next ensure() with different args or at process exit.
}

} // namespace pm::tts


