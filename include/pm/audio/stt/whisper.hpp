#pragma once

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

#include <nlohmann/json.hpp>

namespace pm::stt {

enum class WhisperFlashAttention {
    Auto,
    On,
    Off,
};

struct WhisperLocalOptions {
    std::string language = "auto";
    std::string initial_prompt;
    int n_threads = 0;
    bool use_gpu = true;
    int gpu_device = 0;
    WhisperFlashAttention flash_attn = WhisperFlashAttention::Auto;
};

struct WhisperWord {
    std::string text;
    std::int64_t t0_cs = 0;
    std::int64_t t1_cs = 0;
};

struct WhisperSegment {
    std::string text;
    std::int64_t t0_cs = 0;
    std::int64_t t1_cs = 0;
    std::vector<WhisperWord> words;
    std::string word_map_note;
};

inline bool whisper_segment_repeats(const WhisperSegment& prev, const WhisperSegment& cur)
{
    auto norm = [](const std::string& s) {
        std::size_t b = 0;
        std::size_t e = s.size();
        while (b < e && (s[b] == ' ' || s[b] == '\n' || s[b] == '\r' || s[b] == '\t'))
            ++b;
        while (e > b && (s[e - 1] == ' ' || s[e - 1] == '\n' || s[e - 1] == '\r' || s[e - 1] == '\t'))
            --e;
        return s.substr(b, e - b);
    };
    const auto a = norm(prev.text);
    if (a.empty() || a != norm(cur.text))
        return false;
    return cur.t0_cs == prev.t1_cs || cur.t0_cs == prev.t0_cs + 100;
}

struct WhisperTranscript {
    std::string text;
    std::vector<WhisperSegment> segments;
    bool dtw = false;
};

struct WhisperModelProbe {
    std::string path;
    std::string model_id;
    std::string filename;
    std::string models_dir_override;
    std::vector<std::string> probed;
};

// Host catalog: return true and fill path + filename when this id is installed.
using WhisperCatalogFn = bool (*)(const std::string& model_id,
                                  std::string& path,
                                  std::string& filename);
void set_whisper_catalog(WhisperCatalogFn fn);

std::string whisper_canonical_filename(const std::string& id_or_file);

WhisperModelProbe whisper_probe_model_path(const std::string& model,
                                           const std::string& models_dir = {});
std::string whisper_model_not_found_message(const WhisperModelProbe& probe);
std::string whisper_resolve_model_path(const std::string& model,
                                       const std::string& models_dir = "");

// "auto", an ISO code, an English name, or a small native alias. Unknown -> "auto".
std::string whisper_normalize_language(std::string_view language);

// Host policy. Return false and fill err when local whisper.cpp is disabled.
using WhisperGateFn = bool (*)(std::string& err);
void set_whisper_gate(WhisperGateFn fn);

// Process-wide cancel, OR'd with the per-call atomic passed to transcribe.
using WhisperCancelFn = bool (*)();
void set_whisper_cancel(WhisperCancelFn fn);

// Loads ggml backends that sit beside the host module. No-op when unset.
using GgmlBackendLoadFn = void (*)();
void set_ggml_backend_load(GgmlBackendLoadFn fn);

// Plugin DLL entry points. The host fills this from PmWhisperApi on each call.
struct WhisperBackend {
    std::uint32_t abi_version = 0;
    int (*transcribe)(const float* pcm_16k,
                      std::uint32_t n_samples,
                      const char* model_path,
                      const char* language,
                      int n_threads,
                      int use_gpu,
                      int gpu_device,
                      int flash_attn,
                      const char* prompt,
                      char** text_out,
                      char* err,
                      std::uint32_t err_cap) = nullptr;
    int (*preload)(const char* model_path,
                   int use_gpu,
                   int gpu_device,
                   int flash_attn,
                   char* err,
                   std::uint32_t err_cap) = nullptr;
    int (*cache_info)(char* path_out,
                      std::uint32_t path_cap,
                      int* use_gpu,
                      int* gpu_device) = nullptr;
    int (*unload)(char* err, std::uint32_t err_cap) = nullptr;
    int (*transcribe_segments)(const float* pcm_16k,
                               std::uint32_t n_samples,
                               const char* model_path,
                               const char* language,
                               int n_threads,
                               int use_gpu,
                               int gpu_device,
                               int flash_attn,
                               const char* prompt,
                               char** segments_json,
                               char** text_out,
                               char* err,
                               std::uint32_t err_cap) = nullptr;
    void (*free_result)(void* p) = nullptr;
};

using WhisperResolveFn = bool (*)(WhisperBackend& out);
void set_whisper_resolve(WhisperResolveFn fn);

std::string whisper_local_transcribe(const std::string& wav_path,
                                     const std::string& model_path,
                                     const std::string& language = "auto",
                                     int n_threads = 0);

std::string whisper_local_transcribe_with_options(const std::string& wav_path,
                                                  const std::string& model_path,
                                                  const WhisperLocalOptions& options);

WhisperTranscript whisper_local_transcribe_detailed(
    const std::string& wav_path,
    const std::string& model_path,
    const WhisperLocalOptions& options);

WhisperTranscript whisper_local_transcribe_pcm_detailed(
    const float* pcm_16k,
    std::size_t n_samples,
    const std::string& model_path,
    const WhisperLocalOptions& options,
    const std::atomic_bool* cancel = nullptr);

std::string whisper_local_transcribe_pcm(const float* pcm_16k,
                                         std::size_t n_samples,
                                         const std::string& model_path,
                                         const WhisperLocalOptions& options,
                                         const std::atomic_bool* cancel = nullptr);

void whisper_local_preload(const std::string& model_path,
                           const WhisperLocalOptions& options);

nlohmann::json whisper_loaded_models_json();
bool whisper_unload_loaded_model(std::string_view key, std::string& err);

} // namespace pm::stt
