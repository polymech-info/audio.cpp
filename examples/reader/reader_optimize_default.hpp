#pragma once

// Experimental narration review. Harvest stays immutable.
// One text turn returns JSON. The controller checks printed words, then calls
// image_understand only for a flagged visual claim. Max one look per page.

#include "tts/reader.hpp"

#include <functional>
#include <string>
#include <string_view>

namespace reader_optimize {

struct Preset {
    const char* id;
    int version;
    const char* instructions;
    int previous_pages;
    int next_pages;
    int max_pages;
    // off, on_demand, always. on_demand looks only when a change asks.
    const char* vision;
};

const Preset& default_preset();
const Preset* find(std::string_view id);
std::string ids();

using RewriteFn = std::function<std::string(const std::string& page, const std::string& prompt,
                                             const std::string& system, std::string& err)>;
using VisionFn = std::function<std::string(const std::string& image, const std::string& question, std::string& err)>;

struct RunResult {
    std::string run_dir;
    std::string spoken_path;
    std::string diff_path;
    int reviewed = 0;
    int rewritten = 0;
    int visual_checks = 0;
    int printed_checks = 0;
    int visually_supported = 0;
    int rejected = 0;
    int human_review = 0;
    int vision_calls = 0;
};

bool run(const pm::tts::reader::Store& store, const Preset& preset,
         const RewriteFn& rewrite, const VisionFn& vision,
         RunResult& out, std::string& err);

} // namespace reader_optimize
