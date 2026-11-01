#include "pm/audio/echo.hpp"
#include "pm/audio/log.hpp"

namespace pm::audio {
namespace {

ApmBackend& backend_slot()
{
    static ApmBackend backend;
    return backend;
}

bool backend_ready()
{
    return backend_slot().create != nullptr;
}

} // namespace

void set_apm_backend(const ApmBackend& backend)
{
    backend_slot() = backend;
}

std::unique_ptr<EchoCancellationProcessor> EchoCancellationProcessor::create(Config cfg)
{
    if (!backend_ready())
        return nullptr;
    return std::unique_ptr<EchoCancellationProcessor>(new EchoCancellationProcessor(std::move(cfg)));
}

EchoCancellationProcessor::EchoCancellationProcessor(Config cfg)
    : cfg_(cfg)
{
    const ApmBackend& backend = backend_slot();
    handle_ = backend.create(cfg_.sample_rate_hz, cfg_.channels, cfg_.agc, cfg_.ns);
    active_ = handle_ && backend.active && backend.active(handle_);
    backend_ = active_ ? "webrtc_apm" : "none";
    if (active_)
        pm::audio::log::info(std::string("[audio] echo cancellation active: ") + backend_);
}

EchoCancellationProcessor::~EchoCancellationProcessor()
{
    if (handle_ && backend_slot().destroy)
        backend_slot().destroy(handle_);
}

bool EchoCancellationProcessor::active() const
{
    return active_;
}

const std::string& EchoCancellationProcessor::backend_name() const
{
    return backend_;
}

void EchoCancellationProcessor::set_stream_delay_ms(int ms)
{
    if (handle_ && backend_slot().set_delay_ms)
        backend_slot().set_delay_ms(handle_, ms);
}

void EchoCancellationProcessor::process_render_reference(const int16_t* pcm, std::size_t frames)
{
    if (handle_ && backend_slot().process_render)
        backend_slot().process_render(handle_, pcm, frames);
}

void EchoCancellationProcessor::process_capture(int16_t* pcm, std::size_t frames)
{
    if (handle_ && backend_slot().process_capture)
        backend_slot().process_capture(handle_, pcm, frames);
}

} // namespace pm::audio
