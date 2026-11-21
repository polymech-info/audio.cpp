#include "aec3_artifacts.hpp"

#include <chrono>
#include <cstdint>
#include <ctime>
#include <fstream>
#include <iomanip>
#include <sstream>

namespace pm::audio::aec3 {

std::uint64_t hash_file(const std::filesystem::path& path)
{
    std::ifstream in(path, std::ios::binary);
    if (!in)
        return 0;
    std::uint64_t hash = 14695981039346656037ull;
    char buffer[4096];
    while (in) {
        in.read(buffer, sizeof(buffer));
        const auto n = in.gcount();
        for (std::streamsize i = 0; i < n; ++i) {
            hash ^= static_cast<std::uint8_t>(buffer[i]);
            hash *= 1099511628211ull;
        }
    }
    return hash;
}

bool write_json_file(const std::filesystem::path& path, const nlohmann::json& body)
{
    std::ofstream out(path, std::ios::binary | std::ios::trunc);
    if (!out)
        return false;
    out << body.dump(2) << '\n';
    return static_cast<bool>(out);
}

bool append_jsonl(const std::filesystem::path& path, const nlohmann::json& body)
{
    std::ofstream out(path, std::ios::binary | std::ios::app);
    if (!out)
        return false;
    out << body.dump() << '\n';
    return static_cast<bool>(out);
}

std::string utc_stamp()
{
    const auto now = std::chrono::system_clock::now();
    const std::time_t seconds = std::chrono::system_clock::to_time_t(now);
    std::tm utc{};
#if defined(_WIN32)
    gmtime_s(&utc, &seconds);
#else
    gmtime_r(&seconds, &utc);
#endif
    std::ostringstream text;
    text << std::put_time(&utc, "%Y%m%dT%H%M%SZ");
    return text.str();
}

} // namespace pm::audio::aec3
