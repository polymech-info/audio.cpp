#pragma once

#include <nlohmann/json.hpp>

#include <cstdint>
#include <filesystem>
#include <string>

namespace pm::audio::aec3 {

std::uint64_t hash_file(const std::filesystem::path& path);
bool write_json_file(const std::filesystem::path& path, const nlohmann::json& body);
bool append_jsonl(const std::filesystem::path& path, const nlohmann::json& body);
std::string utc_stamp();

} // namespace pm::audio::aec3
