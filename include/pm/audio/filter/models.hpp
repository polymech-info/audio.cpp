#pragma once

#include <filesystem>
#include <string>
#include <vector>

namespace pm::audio::filter {

struct ModelProbe {
    std::string path;
    std::string hint;
    std::string filename;
    std::vector<std::string> probed;
};

struct RuntimeInfo {
    bool onnx = false;
    bool whisper = false;
    std::string onnx_root;
    std::string whisper_root;
    std::string tanit_root;
};

RuntimeInfo runtime_info();

using OnnxGate = bool (*)(std::string& err);
void set_onnx_gate(OnnxGate gate);
bool onnx_allowed(std::string& err);

void set_model_roots(std::vector<std::filesystem::path> roots);
std::vector<std::filesystem::path> model_search_roots();

ModelProbe probe_deepfilter_model(const std::string& hint);
std::string deepfilter_model_not_found_message(const ModelProbe& probe);

ModelProbe probe_gtcrn_model(const std::string& hint);
std::string gtcrn_model_not_found_message(const ModelProbe& probe);

} // namespace pm::audio::filter
