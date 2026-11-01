#pragma once

#include "pm/audio/capture.hpp"
#include "pm/audio/filter/audio_filter.hpp"
#include "pm/audio/voice/voice_types.hpp"

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <string>

namespace pm::audio::voice {

using CapturePcmCallback = void (*)(void* user, const int16_t* pcm, size_t frames);

struct CaptureAdapter {
    void* context = nullptr;
    bool (*start)(void* context,
                  const pm::audio::MixedCaptureConfig& config,
                  CapturePcmCallback callback,
                  void* callback_user,
                  std::string& error) = nullptr;
    void (*stop)(void* context) = nullptr;
    bool (*is_running)(const void* context) = nullptr;
};

struct EnhancementAdapter {
    void* context = nullptr;
    bool (*enhance)(void* context,
                    const PcmBuffer& input,
                    PcmBuffer& output,
                    const std::atomic_bool& cancelled,
                    std::string& error) = nullptr;
};

struct TranscriptionAdapter {
    void* context = nullptr;
    bool (*transcribe)(void* context,
                       const int16_t* mono_16k_pcm,
                       size_t frames,
                       const std::atomic_bool& cancelled,
                       Transcript& transcript,
                       std::string& error) = nullptr;
};

struct RouteCallbacks {
    void* context = nullptr;
    void (*on_trigger)(void* context, const TriggerEvent& event) = nullptr;
    void (*on_cancel)(void* context, const CancelEvent& event) = nullptr;
    void (*on_state)(void* context, PipelineState state) = nullptr;
    void (*on_error)(void* context, const std::string& error) = nullptr;
    void (*on_transcript)(void* context,
                          const Transcript& transcript,
                          const MatchResult& match) = nullptr;
};

struct VoiceAdapters {
    CaptureAdapter capture;
    EnhancementAdapter enhancement;
    TranscriptionAdapter transcription;
    RouteCallbacks routes;
};

class DeepFilterEnhancer {
public:
    explicit DeepFilterEnhancer(filter::Options options = {});
    ~DeepFilterEnhancer();

    DeepFilterEnhancer(const DeepFilterEnhancer&) = delete;
    DeepFilterEnhancer& operator=(const DeepFilterEnhancer&) = delete;

    EnhancementAdapter adapter() noexcept;

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

class GtcrnEnhancer {
public:
    explicit GtcrnEnhancer(std::string model = {});
    ~GtcrnEnhancer();

    GtcrnEnhancer(const GtcrnEnhancer&) = delete;
    GtcrnEnhancer& operator=(const GtcrnEnhancer&) = delete;

    EnhancementAdapter adapter() noexcept;

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

} // namespace pm::audio::voice
