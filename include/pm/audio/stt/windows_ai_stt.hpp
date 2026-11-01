#pragma once

#include <string>

namespace pm::stt {

// Host reports MSIX package identity. Unset means unpackaged.
using WindowsAiPackagedFn = bool (*)();
void set_windows_ai_packaged(WindowsAiPackagedFn fn);

bool has_package_identity();

enum class WindowsAiSttAvailability {
    Ready,
    UnsupportedOs,
    Unpackaged,
    RuntimeMissing,
    ApiMissing,
};

WindowsAiSttAvailability windows_ai_stt_availability();
const char* windows_ai_stt_availability_message(WindowsAiSttAvailability availability);
std::string windows_ai_stt_transcribe_file(const std::string& wav_path);

} // namespace pm::stt
