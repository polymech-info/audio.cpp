#include "pm/audio/voice/voice_adapters.hpp"

#include "pm/audio/filter/deepfilter.hpp"
#include "pm/audio/filter/gtcrn.hpp"

#include <utility>
#include <vector>

namespace pm::audio::voice {

struct DeepFilterEnhancer::Impl {
    filter::Options options;

    explicit Impl(filter::Options value) : options(std::move(value)) {}

    static bool enhance(void* context,
                        const PcmBuffer& input,
                        PcmBuffer& output,
                        const std::atomic_bool& cancelled,
                        std::string& error)
    {
        auto* self = static_cast<Impl*>(context);
        if (!self) {
            error = "DeepFilter adapter has no context";
            return false;
        }
        if (cancelled.load(std::memory_order_relaxed)) {
            error = "cancelled";
            return false;
        }
        if (input.format.sample_rate != 48000 || input.format.channels == 0) {
            error = "DeepFilter voice input must be 48 kHz PCM";
            return false;
        }

        filter::dfn::StreamSession stream;
        if (!stream.open(self->options, error))
            return false;

        output.format = input.format;
        output.samples.clear();
        output.samples.reserve(input.samples.size());
        stream.process(input.samples.data(), input.frames(), input.format.channels, output.samples);
        if (!cancelled.load(std::memory_order_relaxed))
            stream.flush(output.samples);
        stream.close();

        if (cancelled.load(std::memory_order_relaxed)) {
            output.samples.clear();
            error = "cancelled";
            return false;
        }
        return true;
    }
};

DeepFilterEnhancer::DeepFilterEnhancer(filter::Options options)
    : impl_(std::make_unique<Impl>(std::move(options)))
{
}

DeepFilterEnhancer::~DeepFilterEnhancer() = default;

EnhancementAdapter DeepFilterEnhancer::adapter() noexcept
{
    return EnhancementAdapter{impl_.get(), &Impl::enhance};
}

struct GtcrnEnhancer::Impl {
    std::string model;

    explicit Impl(std::string value) : model(std::move(value)) {}

    static bool enhance(void* context,
                        const PcmBuffer& input,
                        PcmBuffer& output,
                        const std::atomic_bool& cancelled,
                        std::string& error)
    {
        auto* self = static_cast<Impl*>(context);
        if (!self) {
            error = "GTCRN adapter has no context";
            return false;
        }
        if (input.format.sample_rate != 16000 || input.format.channels != 1) {
            error = "GTCRN voice input must be 16 kHz mono PCM";
            return false;
        }
        filter::gtcrn::StreamSession stream;
        if (!stream.open(self->model, error))
            return false;
        output.format = input.format;
        output.samples.clear();
        if (!cancelled.load(std::memory_order_relaxed))
            stream.process(input.samples.data(), input.frames(), output.samples, &cancelled);
        stream.close();
        if (cancelled.load(std::memory_order_relaxed)) {
            output.samples.clear();
            error = "cancelled";
            return false;
        }
        return true;
    }
};

GtcrnEnhancer::GtcrnEnhancer(std::string model)
    : impl_(std::make_unique<Impl>(std::move(model)))
{
}

GtcrnEnhancer::~GtcrnEnhancer() = default;

EnhancementAdapter GtcrnEnhancer::adapter() noexcept
{
    return EnhancementAdapter{impl_.get(), &Impl::enhance};
}

} // namespace pm::audio::voice
