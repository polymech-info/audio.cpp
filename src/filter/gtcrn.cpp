#include "pm/audio/filter/gtcrn.hpp"

namespace pm::audio::filter::gtcrn {

bool available()
{
#if defined(PM_AUDIO_HAS_ONNX) && PM_AUDIO_HAS_ONNX
    return true;
#else
    return false;
#endif
}

std::string backend_name()
{
    return available() ? std::string{"onnx"} : std::string{};
}

Result apply_file(const std::filesystem::path&,
                  const std::filesystem::path&,
                  const Options&)
{
    Result r;
    r.kind = "gtcrn";
    r.error = "gtcrn is reserved (16 kHz live / STT path); "
              "use --filter deepfilter for 48 kHz files";
    return r;
}

} // namespace pm::audio::filter::gtcrn
