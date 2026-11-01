#include "pm/audio/voice/voice_pipeline.hpp"

#include "pm/audio/capture.hpp"
#include "pm/audio/log.hpp"
#include "pm/audio/voice/voice_match.hpp"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
#include <condition_variable>
#include <cstddef>
#include <cstdio>
#include <exception>
#include <mutex>
#include <thread>
#include <utility>
#include <vector>

namespace pm::audio::voice {
namespace {

void append_pcm_16k(const PcmBuffer& input, std::vector<int16_t>& out)
{
    const uint32_t channels = std::max<uint16_t>(1, input.format.channels);
    const uint32_t rate = std::max<uint32_t>(1, input.format.sample_rate);
    const size_t frames = input.frames();
    if (frames == 0 || input.samples.empty())
        return;
    if (rate == 16000 && channels == 1) {
        out.insert(out.end(), input.samples.begin(), input.samples.end());
        return;
    }
    std::vector<int16_t> mono(frames);
    if (channels == 1) {
        std::copy_n(input.samples.data(), frames, mono.begin());
    } else {
        for (size_t i = 0; i < frames; ++i) {
            int sum = 0;
            const size_t base = i * channels;
            for (uint32_t c = 0; c < channels && base + c < input.samples.size(); ++c)
                sum += input.samples[base + c];
            mono[i] = static_cast<int16_t>(sum / static_cast<int>(channels));
        }
    }
    if (rate == 16000) {
        out.insert(out.end(), mono.begin(), mono.end());
        return;
    }
    const double ratio = 16000.0 / static_cast<double>(rate);
    const size_t out_n = static_cast<size_t>(static_cast<double>(mono.size()) * ratio);
    out.reserve(out.size() + out_n);
    for (size_t i = 0; i < out_n; ++i) {
        const double src = static_cast<double>(i) / ratio;
        const size_t i0 = std::min(static_cast<size_t>(src), mono.size() - 1);
        const size_t i1 = std::min(i0 + 1, mono.size() - 1);
        const float fr = static_cast<float>(src - static_cast<double>(i0));
        const float mixed = static_cast<float>(mono[i0]) * (1.f - fr) +
                            static_cast<float>(mono[i1]) * fr;
        out.push_back(static_cast<int16_t>(std::clamp(mixed, -32768.f, 32767.f)));
    }
}



uint64_t unix_time_ms()
{
    return static_cast<uint64_t>(std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::system_clock::now().time_since_epoch()).count());
}

uint64_t frames_for_ms(uint32_t rate, size_t milliseconds)
{
    return (static_cast<uint64_t>(rate) * milliseconds + 999) / 1000;
}

void mix_filter_blend(const PcmBuffer& dry, PcmBuffer& wet, float blend)
{
    blend = clamp_filter_blend(blend);
    if (blend >= 1.0f)
        return;
    if (blend <= 0.0f || wet.samples.empty()) {
        wet = dry;
        return;
    }
    if (dry.samples.empty())
        return;
    const size_t n = std::min(dry.samples.size(), wet.samples.size());
    const float wet_w = blend;
    const float dry_w = 1.0f - blend;
    for (size_t i = 0; i < n; ++i) {
        const float mixed =
            static_cast<float>(dry.samples[i]) * dry_w +
            static_cast<float>(wet.samples[i]) * wet_w;
        wet.samples[i] = static_cast<int16_t>(
            std::clamp(mixed, -32768.0f, 32767.0f));
    }
    if (wet.samples.size() < dry.samples.size()) {
        wet.samples.insert(wet.samples.end(),
            dry.samples.begin() + static_cast<std::ptrdiff_t>(n),
            dry.samples.end());
    } else {
        wet.samples.resize(dry.samples.size());
    }
    wet.format = dry.format;
}

float pcm_rms(const int16_t* pcm, size_t frames, uint16_t channels)
{
    const size_t count = frames * channels;
    if (!pcm || count == 0)
        return 0.0f;
    double sum = 0.0;
    for (size_t i = 0; i < count; ++i) {
        const double sample = pcm[i];
        sum += sample * sample;
    }
    return static_cast<float>(std::sqrt(sum / static_cast<double>(count)));
}

} // namespace

struct VoicePipeline::Impl {
    PipelineConfig config;
    VoiceAdapters adapters;

    mutable std::mutex mutex;
    std::condition_variable wake;
    std::condition_variable idle;
    std::vector<int16_t> ring;
    size_t ring_read = 0;
    size_t ring_size = 0;

    std::thread worker;
    std::atomic<PipelineState> current_state{PipelineState::stopped};
    std::atomic_bool accepting{false};
    std::atomic_bool stop_requested{false};
    std::atomic_bool finish_requested{false};
    std::atomic_bool cancel_requested{false};
    std::atomic_bool job_active{false};
    std::atomic_bool worker_busy{false};
    std::atomic_bool utterance_active{false};
    std::atomic_bool capture_started{false};

    std::atomic<uint64_t> callback_frames{0};
    std::atomic<uint64_t> dropped_frames{0};
    std::atomic<uint64_t> completed_utterances{0};
    std::atomic<uint64_t> rejected_utterances{0};
    std::atomic<uint64_t> next_id{1};

    std::vector<int16_t> preroll;
    std::vector<int16_t> utterance;
    uint64_t speech_run_frames = 0;
    uint64_t silence_frames = 0;

    Impl(PipelineConfig value, VoiceAdapters value_adapters)
        : config(std::move(value)), adapters(value_adapters)
    {
    }

    void notify_state(PipelineState value) noexcept
    {
        current_state.store(value, std::memory_order_release);
        try {
            if (adapters.routes.on_state)
                adapters.routes.on_state(adapters.routes.context, value);
        } catch (...) {
        }
    }

    void notify_error(const std::string& error) noexcept
    {
        try {
            if (adapters.routes.on_error)
                adapters.routes.on_error(adapters.routes.context, error);
        } catch (...) {
        }
    }

    std::string event_id(const char* prefix)
    {
        char value[64]{};
        std::snprintf(value, sizeof(value), "%s-%llu-%llu", prefix,
                      static_cast<unsigned long long>(unix_time_ms()),
                      static_cast<unsigned long long>(
                          next_id.fetch_add(1, std::memory_order_relaxed)));
        return value;
    }

    void emit_cancel(std::string reason)
    {
        CancelEvent event;
        event.id = event_id("voice-cancel");
        event.timestamp_ms = unix_time_ms();
        event.reason = std::move(reason);
        try {
            if (adapters.routes.on_cancel)
                adapters.routes.on_cancel(adapters.routes.context, event);
        } catch (const std::exception& e) {
            notify_error(std::string("voice cancel route failed: ") + e.what());
        } catch (...) {
            notify_error("voice cancel route failed");
        }
    }

    void emit_transcript(const Transcript& transcript, const MatchResult& match)
    {
        try {
            if (adapters.routes.on_transcript)
                adapters.routes.on_transcript(adapters.routes.context, transcript, match);
        } catch (const std::exception& e) {
            notify_error(std::string("voice transcript route failed: ") + e.what());
        } catch (...) {
            notify_error("voice transcript route failed");
        }
    }

    void route_transcript(const Transcript& transcript,
                          std::string effective_filter = {},
                          std::string filter_fallback = {})
    {
        const MatchResult match = match_voice_text(transcript.text, config.match);
        pm::audio::log::info(std::string("[voice] heard \"") + transcript.text
                     + "\" wake=" + match.wake_prefix
                     + " command=" + match.command_text
                     + (match.control != ControlIntent::none
                            ? std::string(" control=") + control_intent_name(match.control)
                            : std::string()));
        emit_transcript(transcript, match);
        if (!match.matched)
            return;
        if (match.control != ControlIntent::none) {
            emit_cancel(control_intent_name(match.control));
            if (job_active.load(std::memory_order_acquire) ||
                utterance_active.load(std::memory_order_acquire)) {
                cancel_requested.store(true, std::memory_order_release);
            }
            if (match.control == ControlIntent::stop_listening) {
                accepting.store(false, std::memory_order_release);
                stop_requested.store(true, std::memory_order_release);
                wake.notify_all();
                if (capture_started.exchange(false, std::memory_order_acq_rel) &&
                    adapters.capture.stop) {
                    adapters.capture.stop(adapters.capture.context);
                }
            }
            return;
        }

        TriggerEvent event;
        event.id = event_id("voice-trigger");
        event.timestamp_ms = unix_time_ms();
        event.route = config.route;
        event.text = match.command_text;
        event.raw_text = transcript.text;
        event.wake_prefix = match.wake_prefix;
        event.confidence = transcript.confidence;
        event.language = transcript.language;
        event.requested_filter = config.requested_filter;
        event.effective_filter = effective_filter.empty()
            ? config.effective_filter : std::move(effective_filter);
        event.filter_fallback = std::move(filter_fallback);
        try {
            if (adapters.routes.on_trigger)
                adapters.routes.on_trigger(adapters.routes.context, event);
        } catch (const std::exception& e) {
            notify_error(std::string("voice trigger route failed: ") + e.what());
        } catch (...) {
            notify_error("voice trigger route failed");
        }
    }

    void complete_utterance(PcmBuffer input)
    {
        const double seconds = static_cast<double>(input.frames())
            / static_cast<double>(std::max<uint32_t>(1, input.format.sample_rate));
        if (input.frames() < frames_for_ms(input.format.sample_rate,
                                            config.vad.min_utterance_ms)) {
            pm::audio::log::debug("[voice] drop short utterance seconds=" + std::to_string(seconds));
            rejected_utterances.fetch_add(1, std::memory_order_relaxed);
            return;
        }

        pm::audio::log::debug("[voice] utterance ready seconds=" + std::to_string(seconds)
                      + " frames=" + std::to_string(input.frames()));
        job_active.store(true, std::memory_order_release);
        notify_state(PipelineState::processing);

        PcmBuffer enhanced;
        const PcmBuffer* selected = &input;
        std::string error;
        std::string effective_filter = config.effective_filter;
        std::string filter_fallback;
        const float blend = clamp_filter_blend(config.filter_blend);
        if (config.enhance_completed_utterances && adapters.enhancement.enhance
            && blend > 0.0f) {
            if (!adapters.enhancement.enhance(adapters.enhancement.context, input, enhanced,
                                              cancel_requested, error)) {
                if (cancel_requested.load(std::memory_order_acquire)) {
                    rejected_utterances.fetch_add(1, std::memory_order_relaxed);
                    finish_job();
                    return;
                }
                if (config.enhancement_required) {
                    notify_error("voice enhancement failed: " + error);
                    rejected_utterances.fetch_add(1, std::memory_order_relaxed);
                    finish_job();
                    return;
                }
                filter_fallback = error.empty() ? "enhancer unavailable" : error;
                effective_filter = "off";
                notify_error("voice enhancement unavailable; using dry utterance: " +
                             filter_fallback);
            } else {
                if (blend < 1.0f)
                    mix_filter_blend(input, enhanced, blend);
                selected = &enhanced;
            }
        }

        if (cancel_requested.load(std::memory_order_acquire)) {
            rejected_utterances.fetch_add(1, std::memory_order_relaxed);
            finish_job();
            return;
        }

        std::vector<int16_t> mono_16k;
        append_pcm_16k(*selected, mono_16k);

        Transcript transcript;
        if (!adapters.transcription.transcribe) {
            notify_error("voice transcription adapter is not configured");
            rejected_utterances.fetch_add(1, std::memory_order_relaxed);
            finish_job();
            return;
        }
        pm::audio::log::debug("[voice] transcribing frames=" + std::to_string(mono_16k.size()));
        if (!adapters.transcription.transcribe(
                adapters.transcription.context, mono_16k.data(), mono_16k.size(),
                cancel_requested, transcript, error)) {
            if (!cancel_requested.load(std::memory_order_acquire))
                notify_error("voice transcription failed: " + error);
            rejected_utterances.fetch_add(1, std::memory_order_relaxed);
            finish_job();
            return;
        }
        if (transcript.text.empty()) {
            pm::audio::log::debug("[voice] no speech");
            rejected_utterances.fetch_add(1, std::memory_order_relaxed);
            finish_job();
            return;
        }
        if (!cancel_requested.load(std::memory_order_acquire))
            route_transcript(transcript, std::move(effective_filter),
                             std::move(filter_fallback));
        completed_utterances.fetch_add(1, std::memory_order_relaxed);
        finish_job();
    }

    void finish_job()
    {
        cancel_requested.store(false, std::memory_order_release);
        job_active.store(false, std::memory_order_release);
        if (!stop_requested.load(std::memory_order_acquire))
            notify_state(PipelineState::listening);
        idle.notify_all();
    }

    void trim_preroll()
    {
        const size_t channels = config.capture_format.channels;
        const size_t keep = static_cast<size_t>(
            frames_for_ms(config.capture_format.sample_rate, config.vad.preroll_ms)) * channels;
        if (keep == 0) {
            preroll.clear();
        } else if (preroll.size() > keep) {
            preroll.erase(preroll.begin(),
                          preroll.begin() + static_cast<std::ptrdiff_t>(preroll.size() - keep));
        }
    }

    void reset_segment()
    {
        utterance.clear();
        speech_run_frames = 0;
        silence_frames = 0;
        utterance_active.store(false, std::memory_order_release);
    }

    void finalize_segment()
    {
        if (!utterance_active.load(std::memory_order_acquire) || utterance.empty()) {
            reset_segment();
            idle.notify_all();
            return;
        }
        PcmBuffer completed;
        completed.format = config.capture_format;
        completed.samples.swap(utterance);
        reset_segment();
        complete_utterance(std::move(completed));
    }

    void consume_pcm(const int16_t* pcm, size_t frames)
    {
        if (!pcm || frames == 0)
            return;
        const size_t channels = config.capture_format.channels;
        const size_t samples = frames * channels;

        if (cancel_requested.load(std::memory_order_acquire) &&
            utterance_active.load(std::memory_order_acquire)) {
            reset_segment();
            cancel_requested.store(false, std::memory_order_release);
        }

        const float rms = pcm_rms(pcm, frames, config.capture_format.channels);
        const bool speech = rms >= config.vad.rms_threshold;
        if (!utterance_active.load(std::memory_order_acquire)) {
            preroll.insert(preroll.end(), pcm, pcm + samples);
            trim_preroll();
            speech_run_frames = speech ? speech_run_frames + frames : 0;
            if (speech_run_frames >=
                frames_for_ms(config.capture_format.sample_rate, config.vad.speech_start_ms)) {
                utterance.swap(preroll);
                utterance_active.store(true, std::memory_order_release);
                silence_frames = 0;
                pm::audio::log::debug("[voice] speech started rms=" + std::to_string(rms));
            }
            return;
        }

        utterance.insert(utterance.end(), pcm, pcm + samples);
        silence_frames = speech ? 0 : silence_frames + frames;
        const uint64_t utterance_frames = utterance.size() / channels;
        if (silence_frames >= frames_for_ms(config.capture_format.sample_rate,
                                             config.vad.silence_ms) ||
            utterance_frames >= frames_for_ms(config.capture_format.sample_rate,
                                               config.vad.max_utterance_ms)) {
            finalize_segment();
        }
    }

    void worker_loop()
    {
        const size_t channels = config.capture_format.channels;
        const size_t chunk_samples = std::max<size_t>(
            channels, static_cast<size_t>(
                frames_for_ms(config.capture_format.sample_rate, config.worker_chunk_ms)) *
                channels);
        std::vector<int16_t> chunk(chunk_samples);

        for (;;) {
            size_t copied = 0;
            bool finish = false;
            {
                std::unique_lock lock(mutex);
                wake.wait(lock, [&] {
                    return ring_size != 0 ||
                           finish_requested.load(std::memory_order_acquire) ||
                           stop_requested.load(std::memory_order_acquire);
                });
                copied = std::min(ring_size, chunk.size());
                for (size_t i = 0; i < copied; ++i)
                    chunk[i] = ring[(ring_read + i) % ring.size()];
                ring_read = ring.empty() ? 0 : (ring_read + copied) % ring.size();
                ring_size -= copied;
                const bool requested =
                    finish_requested.exchange(false, std::memory_order_acq_rel);
                finish = requested && ring_size == 0;
                if (requested && !finish)
                    finish_requested.store(true, std::memory_order_release);
                worker_busy.store(copied != 0 || finish, std::memory_order_release);
            }
            try {
                if (copied != 0)
                    consume_pcm(chunk.data(), copied / channels);
                if (finish)
                    finalize_segment();
            } catch (const std::exception& e) {
                rejected_utterances.fetch_add(1, std::memory_order_relaxed);
                reset_segment();
                job_active.store(false, std::memory_order_release);
                cancel_requested.store(false, std::memory_order_release);
                notify_error(std::string("voice worker failed: ") + e.what());
            } catch (...) {
                rejected_utterances.fetch_add(1, std::memory_order_relaxed);
                reset_segment();
                job_active.store(false, std::memory_order_release);
                cancel_requested.store(false, std::memory_order_release);
                notify_error("voice worker failed");
            }
            worker_busy.store(false, std::memory_order_release);

            if (copied == 0 && stop_requested.load(std::memory_order_acquire)) {
                if (config.flush_utterance_on_stop)
                    finalize_segment();
                else
                    reset_segment();
                break;
            }
            idle.notify_all();
        }
        job_active.store(false, std::memory_order_release);
        accepting.store(false, std::memory_order_release);
        notify_state(PipelineState::stopped);
        idle.notify_all();
    }

    bool start_worker(std::string& error)
    {
        if (accepting.load(std::memory_order_acquire)) {
            error = "voice pipeline is already running";
            return false;
        }
        if (worker.joinable())
            worker.join();
        if (config.capture_format.sample_rate < 8000 ||
            config.capture_format.channels == 0 ||
            config.capture_format.channels > 2) {
            error = "voice capture format must be 8 kHz or higher with one or two channels";
            return false;
        }
        const uint64_t capacity_frames = frames_for_ms(
            config.capture_format.sample_rate, std::max<size_t>(1, config.callback_queue_ms));
        ring.assign(static_cast<size_t>(capacity_frames) * config.capture_format.channels, 0);
        ring_read = 0;
        ring_size = 0;
        preroll.clear();
        reset_segment();
        stop_requested.store(false, std::memory_order_release);
        finish_requested.store(false, std::memory_order_release);
        cancel_requested.store(false, std::memory_order_release);
        accepting.store(true, std::memory_order_release);
        notify_state(PipelineState::starting);
        try {
            worker = std::thread([this] { worker_loop(); });
        } catch (const std::exception& e) {
            accepting.store(false, std::memory_order_release);
            notify_state(PipelineState::failed);
            error = std::string("could not start voice worker: ") + e.what();
            return false;
        }
        notify_state(PipelineState::listening);
        return true;
    }

    bool enqueue(const int16_t* pcm, size_t frames) noexcept
    {
        if (!accepting.load(std::memory_order_acquire) || !pcm || frames == 0)
            return false;
        callback_frames.fetch_add(frames, std::memory_order_relaxed);
        const size_t count = frames * config.capture_format.channels;
        std::unique_lock lock(mutex, std::try_to_lock);
        if (job_active.load(std::memory_order_acquire)) {
            dropped_frames.fetch_add(frames, std::memory_order_relaxed);
            return false;
        }
        if (!lock.owns_lock() || count > ring.size() - ring_size) {
            const auto dropped = dropped_frames.fetch_add(frames, std::memory_order_relaxed) + frames;
            if (dropped == frames || dropped % 8000 < frames)
                pm::audio::log::debug("[voice] dropped capture frames total=" + std::to_string(dropped));
            return false;
        }
        const size_t write = (ring_read + ring_size) % ring.size();
        for (size_t i = 0; i < count; ++i)
            ring[(write + i) % ring.size()] = pcm[i];
        ring_size += count;
        lock.unlock();
        wake.notify_one();
        return true;
    }
};

VoicePipeline::VoicePipeline(PipelineConfig config, VoiceAdapters adapters)
    : impl_(std::make_unique<Impl>(std::move(config), adapters))
{
}

VoicePipeline::~VoicePipeline()
{
    stop();
}

bool VoicePipeline::start(const MixedCaptureConfig& capture_config, std::string& error)
{
    if (!impl_->adapters.capture.start || !impl_->adapters.capture.stop) {
        error = "voice capture adapter is not configured";
        return false;
    }
    MixedCaptureConfig resolved = capture_config;
    if (resolved.sample_rate == 0)
        resolved.sample_rate = impl_->config.capture_format.sample_rate;
    if (resolved.channels == 0)
        resolved.channels = impl_->config.capture_format.channels;
    uint32_t sample_rate = 0;
    uint32_t channels = 0;
    resolve_capture_pcm_format(resolved, sample_rate, channels);
    impl_->config.capture_format.sample_rate = sample_rate;
    impl_->config.capture_format.channels = static_cast<uint16_t>(channels);
    if (!impl_->start_worker(error))
        return false;
    if (!impl_->adapters.capture.start(impl_->adapters.capture.context, resolved,
                                       [](void* user, const int16_t* pcm, size_t frames) {
                                           static_cast<VoicePipeline*>(user)->inject_pcm(pcm, frames);
                                       },
                                       this, error)) {
        impl_->notify_state(PipelineState::failed);
        stop();
        return false;
    }
    impl_->capture_started.store(true, std::memory_order_release);
    pm::audio::log::info("[voice] capture started rate=" + std::to_string(sample_rate)
                 + " channels=" + std::to_string(channels)
                 + " source=" + std::string(capture_source_name(resolved.source)));
    return true;
}

bool VoicePipeline::start_replay(std::string& error)
{
    return impl_->start_worker(error);
}

void VoicePipeline::stop()
{
    if (!impl_->worker.joinable() &&
        !impl_->capture_started.load(std::memory_order_acquire))
        return;
    if (impl_->capture_started.exchange(false, std::memory_order_acq_rel) &&
        impl_->adapters.capture.stop) {
        impl_->adapters.capture.stop(impl_->adapters.capture.context);
    }
    impl_->accepting.store(false, std::memory_order_release);
    impl_->stop_requested.store(true, std::memory_order_release);
    impl_->cancel_requested.store(true, std::memory_order_release);
    if (state() != PipelineState::stopped)
        impl_->notify_state(PipelineState::stopping);
    impl_->wake.notify_all();
    if (impl_->worker.joinable() && impl_->worker.get_id() != std::this_thread::get_id())
        impl_->worker.join();
}

bool VoicePipeline::is_running() const noexcept
{
    return impl_->accepting.load(std::memory_order_acquire);
}

PipelineState VoicePipeline::state() const noexcept
{
    return impl_->current_state.load(std::memory_order_acquire);
}

bool VoicePipeline::inject_pcm(const int16_t* interleaved_pcm, size_t frames) noexcept
{
    return impl_->enqueue(interleaved_pcm, frames);
}

bool VoicePipeline::replay_pcm(const PcmBuffer& pcm,
                               std::chrono::milliseconds timeout,
                               std::string& error)
{
    if (!is_running()) {
        error = "voice replay worker is not running";
        return false;
    }
    if (pcm.format.sample_rate != impl_->config.capture_format.sample_rate ||
        pcm.format.channels != impl_->config.capture_format.channels) {
        error = "voice replay PCM format does not match the configured capture format";
        return false;
    }

    const auto deadline = std::chrono::steady_clock::now() + timeout;
    const size_t channels = pcm.format.channels;
    const size_t block_frames = std::max<size_t>(
        1, std::min(impl_->ring.size() / channels,
                    static_cast<size_t>(
                        frames_for_ms(pcm.format.sample_rate,
                                      impl_->config.worker_chunk_ms))));
    size_t frame_offset = 0;
    while (frame_offset < pcm.frames()) {
        const size_t frames = std::min(block_frames, pcm.frames() - frame_offset);
        const size_t count = frames * channels;
        std::unique_lock lock(impl_->mutex);
        if (!impl_->idle.wait_until(lock, deadline, [&] {
                return !impl_->accepting.load(std::memory_order_acquire) ||
                       count <= impl_->ring.size() - impl_->ring_size;
            })) {
            error = "voice replay timed out waiting for PCM queue space";
            return false;
        }
        if (!impl_->accepting.load(std::memory_order_acquire)) {
            error = "voice replay worker stopped";
            return false;
        }
        const size_t write = (impl_->ring_read + impl_->ring_size) % impl_->ring.size();
        const int16_t* source = pcm.samples.data() + frame_offset * channels;
        for (size_t i = 0; i < count; ++i)
            impl_->ring[(write + i) % impl_->ring.size()] = source[i];
        impl_->ring_size += count;
        impl_->callback_frames.fetch_add(frames, std::memory_order_relaxed);
        frame_offset += frames;
        lock.unlock();
        impl_->wake.notify_one();
    }

    finish_injected_utterance();
    const auto now = std::chrono::steady_clock::now();
    if (now >= deadline || !wait_idle(
            std::chrono::duration_cast<std::chrono::milliseconds>(deadline - now))) {
        error = "voice replay timed out waiting for utterance processing";
        return false;
    }
    return true;
}

void VoicePipeline::finish_injected_utterance() noexcept
{
    impl_->finish_requested.store(true, std::memory_order_release);
    impl_->wake.notify_one();
}

void VoicePipeline::inject_text(const Transcript& transcript)
{
    impl_->route_transcript(transcript);
}

void VoicePipeline::cancel_current(std::string reason)
{
    if (impl_->job_active.load(std::memory_order_acquire) ||
        impl_->utterance_active.load(std::memory_order_acquire)) {
        impl_->cancel_requested.store(true, std::memory_order_release);
    }
    impl_->emit_cancel(std::move(reason));
    impl_->wake.notify_all();
}

bool VoicePipeline::wait_idle(std::chrono::milliseconds timeout)
{
    std::unique_lock lock(impl_->mutex);
    return impl_->idle.wait_for(lock, timeout, [&] {
        return impl_->ring_size == 0 &&
               !impl_->worker_busy.load(std::memory_order_acquire) &&
               !impl_->utterance_active.load(std::memory_order_acquire) &&
               !impl_->job_active.load(std::memory_order_acquire);
    });
}

PipelineStats VoicePipeline::stats() const noexcept
{
    return PipelineStats{
        impl_->callback_frames.load(std::memory_order_relaxed),
        impl_->dropped_frames.load(std::memory_order_relaxed),
        impl_->completed_utterances.load(std::memory_order_relaxed),
        impl_->rejected_utterances.load(std::memory_order_relaxed),
    };
}

} // namespace pm::audio::voice

