#pragma once

#include <filesystem>
#include <string>

namespace pm::audio {

inline std::string path_u8(const std::filesystem::path& path)
{
    const auto bytes = path.u8string();
    return std::string(reinterpret_cast<const char*>(bytes.data()), bytes.size());
}

std::filesystem::path exe_directory();

} // namespace pm::audio
