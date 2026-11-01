#pragma once

#include <cstdint>
#include <string>
#include <vector>

namespace pm::tts {

std::vector<uint8_t> windows_tts_synthesize(const std::string& text, const std::string& voice);

} // namespace pm::tts
