#include "pm/audio/echo.hpp"

#if defined(_MSC_VER)
#pragma warning(push)
#pragma warning(disable : 4068)
#endif
#include "api/audio/audio_processing.h"
#include "api/audio/builtin_audio_processing_builder.h"
#include "api/environment/environment_factory.h"
#if defined(_MSC_VER)
#pragma warning(pop)
#endif

#include <algorithm>
#include <cstdint>
#include <mutex>
#include <vector>

namespace {

struct State {
    std::mutex mutex;
    webrtc::scoped_refptr<webrtc::AudioProcessing> apm;
    webrtc::StreamConfig stream{16000, 1};
    std::vector<std::int16_t> reverse_out;
    std::vector<std::int16_t> capture_out;
    std::size_t frame_size = 0;
    int delay_ms = 0;
    bool delay_set = false;
};

void fill_statistics(const webrtc::AudioProcessingStats& stats, pm::audio::ApmStatistics* out)
{
    if (stats.echo_return_loss) {
        out->has_echo_return_loss = true;
        out->echo_return_loss = *stats.echo_return_loss;
    }
    if (stats.echo_return_loss_enhancement) {
        out->has_echo_return_loss_enhancement = true;
        out->echo_return_loss_enhancement = *stats.echo_return_loss_enhancement;
    }
    if (stats.divergent_filter_fraction) {
        out->has_divergent_filter_fraction = true;
        out->divergent_filter_fraction = *stats.divergent_filter_fraction;
    }
    if (stats.delay_median_ms) {
        out->has_delay_median_ms = true;
        out->delay_median_ms = *stats.delay_median_ms;
    }
    if (stats.delay_standard_deviation_ms) {
        out->has_delay_standard_deviation_ms = true;
        out->delay_standard_deviation_ms = *stats.delay_standard_deviation_ms;
    }
    if (stats.residual_echo_likelihood) {
        out->has_residual_echo_likelihood = true;
        out->residual_echo_likelihood = *stats.residual_echo_likelihood;
    }
    if (stats.residual_echo_likelihood_recent_max) {
        out->has_residual_echo_likelihood_recent_max = true;
        out->residual_echo_likelihood_recent_max = *stats.residual_echo_likelihood_recent_max;
    }
    if (stats.delay_ms) {
        out->has_delay_ms = true;
        out->delay_ms = *stats.delay_ms;
    }
}

pm::audio::ApmBackend make_backend()
{
    pm::audio::ApmBackend backend;
    backend.create = [](int rate, int channels, bool agc, bool ns) -> void* {
        if (rate <= 0 || channels != 1)
            return nullptr;
        auto* state = new State();
        webrtc::AudioProcessing::Config config;
        config.echo_canceller.enabled = true;
        config.high_pass_filter.enabled = true;
        config.noise_suppression.enabled = ns;
        config.gain_controller2.enabled = agc;
        state->stream = webrtc::StreamConfig(rate, channels);
        webrtc::ProcessingConfig processing;
        processing.input_stream() = state->stream;
        processing.output_stream() = state->stream;
        processing.reverse_input_stream() = state->stream;
        processing.reverse_output_stream() = state->stream;
        webrtc::Environment env = webrtc::CreateEnvironment();
        state->apm = webrtc::BuiltinAudioProcessingBuilder(config).Build(env);
        if (!state->apm || state->apm->Initialize(processing) != 0) {
            delete state;
            return nullptr;
        }
        state->frame_size = static_cast<std::size_t>(webrtc::AudioProcessing::GetFrameSize(rate));
        state->reverse_out.resize(state->frame_size);
        state->capture_out.resize(state->frame_size);
        return state;
    };
    backend.destroy = [](void* handle) { delete static_cast<State*>(handle); };
    backend.set_delay_ms = [](void* handle, int ms) {
        auto* state = static_cast<State*>(handle);
        if (!state)
            return;
        std::lock_guard<std::mutex> lock(state->mutex);
        state->delay_ms = ms;
        state->delay_set = true;
        if (state->apm)
            state->apm->set_stream_delay_ms(ms);
    };
    backend.process_render = [](void* handle, const std::int16_t* pcm, std::size_t frames) {
        auto* state = static_cast<State*>(handle);
        if (!state || !state->apm || !pcm || frames == 0 || state->frame_size == 0)
            return;
        std::lock_guard<std::mutex> lock(state->mutex);
        for (std::size_t offset = 0; offset + state->frame_size <= frames; offset += state->frame_size) {
            if (state->apm->ProcessReverseStream(pcm + offset, state->stream, state->stream, state->reverse_out.data()) != 0)
                return;
        }
    };
    backend.process_capture = [](void* handle, std::int16_t* pcm, std::size_t frames) {
        auto* state = static_cast<State*>(handle);
        if (!state || !state->apm || !pcm || frames == 0 || state->frame_size == 0)
            return;
        std::lock_guard<std::mutex> lock(state->mutex);
        if (state->delay_set)
            state->apm->set_stream_delay_ms(state->delay_ms);
        for (std::size_t offset = 0; offset + state->frame_size <= frames; offset += state->frame_size) {
            if (state->apm->ProcessStream(pcm + offset, state->stream, state->stream, state->capture_out.data()) != 0)
                return;
            std::copy(state->capture_out.begin(), state->capture_out.end(), pcm + offset);
        }
    };
    backend.active = [](void* handle) {
        return handle && static_cast<State*>(handle)->apm;
    };
    backend.statistics = [](void* handle, pm::audio::ApmStatistics* out) {
        auto* state = static_cast<State*>(handle);
        if (!state || !state->apm || !out)
            return false;
        std::lock_guard<std::mutex> lock(state->mutex);
        fill_statistics(state->apm->GetStatistics(), out);
        return true;
    };
    return backend;
}

} // namespace

void install_aec3_webrtc_backend()
{
    pm::audio::set_apm_backend(make_backend());
}
