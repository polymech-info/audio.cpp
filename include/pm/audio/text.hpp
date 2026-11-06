#pragma once

// UTF-8 paths and ASCII case folding used by this library.
// Inline so a checkout of audio.cpp does not need packages/commons.cpp.
// The Tanit image keeps its own copies for host code. The functions match that behavior.

#include <cstdint>
#include <filesystem>
#include <string>
#include <string_view>

namespace pm::audio {

inline std::string lower_ascii(std::string s)
{
    for (char& c : s) {
        if (c >= 'A' && c <= 'Z')
            c = static_cast<char>(c - 'A' + 'a');
    }
    return s;
}

inline std::string path_to_utf8(const std::filesystem::path& path)
{
    const auto utf8 = path.u8string();
    return std::string(reinterpret_cast<const char*>(utf8.data()), utf8.size());
}

inline std::filesystem::path path_from_utf8(std::string_view text)
{
    const std::u8string utf8(reinterpret_cast<const char8_t*>(text.data()), text.size());
    return std::filesystem::path(utf8);
}

} // namespace pm::audio
