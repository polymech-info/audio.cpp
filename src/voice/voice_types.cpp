#include "pm/audio/voice/voice_types.hpp"

namespace pm::audio::voice {

const char* control_intent_name(ControlIntent value) noexcept
{
    switch (value) {
        case ControlIntent::cancel:         return "cancel";
        case ControlIntent::stop_listening: return "stop_listening";
        default:                            return "none";
    }
}

const char* pipeline_state_name(PipelineState value) noexcept
{
    switch (value) {
        case PipelineState::starting:   return "starting";
        case PipelineState::listening:  return "listening";
        case PipelineState::processing: return "processing";
        case PipelineState::stopping:   return "stopping";
        case PipelineState::failed:     return "failed";
        default:                        return "stopped";
    }
}

} // namespace pm::audio::voice
