#pragma once

#include <cstdint>
#include <filesystem>
#include <string>
#include <string_view>

namespace pm::audio::filter {

enum class Kind {
    DeepFilter,
    Gtcrn,
};

struct Options {
    Kind        kind = Kind::DeepFilter;
    std::string model;
    float       atten_lim_db = 100.f;
    bool        post_filter = false;
    bool        compensate_delay = true;
};

struct Result {
    bool        ok = false;
    std::string error;
    std::string kind;
    std::string backend;
    std::string model;
    uint32_t    sample_rate = 0;
    uint16_t    channels    = 0;
    uint64_t    frames      = 0;
};

Kind parse_kind(std::string_view token);
const char* kind_name(Kind kind) noexcept;

inline bool wants_filter(std::string_view token, bool post_filter = false,
                         const std::string& model = {})
{
    if (post_filter || !model.empty())
        return true;
    return !token.empty() && token != "off" && token != "none";
}

inline Options make_options(std::string_view token,
                            std::string model = {},
                            float atten_lim_db = 100.f,
                            bool post_filter = false,
                            bool compensate_delay = true)
{
    Options o;
    o.kind = parse_kind(token.empty() ? "deepfilter" : token);
    o.model = std::move(model);
    o.atten_lim_db = atten_lim_db;
    o.post_filter = post_filter;
    o.compensate_delay = compensate_delay;
    return o;
}

bool available(Kind kind);
std::string available_backend(Kind kind);

Result decode_to_wav(const std::filesystem::path& src,
                     const std::filesystem::path& dst,
                     uint32_t sample_rate = 16000);

Result apply_file(const std::filesystem::path& src,
                  const std::filesystem::path& dst,
                  const Options& opt = {});

Result apply_file_inplace(const std::filesystem::path& path,
                          const Options& opt = {});

std::int64_t duration_ms(const std::filesystem::path& src);

} // namespace pm::audio::filter
