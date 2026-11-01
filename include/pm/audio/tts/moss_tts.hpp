#pragma once

#include <filesystem>
#include <functional>
#include <mutex>
#include <string>
#include <string_view>
#include <vector>

#include <nlohmann/json.hpp>

namespace pm::tts {

using MossPcmCallback =
    std::function<bool(const float* interleaved, int frames, int channels)>;

// Extra DLL search directories. Tanit uses this for plugins/moss and dist/bin.
using MossSearchFn = void (*)(std::vector<std::filesystem::path>& dirs);
void set_moss_search(MossSearchFn fn);

// Soft-loads moss-tts.dll. The DLL is not linked.
class MossNanoEngine {
public:
    static MossNanoEngine& global();

    int ensure(const std::string& model,
               const std::string& codec,
               const std::string& tokenizer);

    int tts(const std::string& text,
            const std::string& ref_audio,
            const std::string& out_wav,
            int seed = 0);

    int stream(const std::string& text,
               const std::string& ref_audio,
               const MossPcmCallback& callback,
               int& sample_rate,
               int seed = 0);

    void unload();
    bool is_loaded() const;
    std::string last_error() const;
    nlohmann::json loaded_models_json() const;
    bool unload_loaded_model(std::string_view key, std::string& err);

    ~MossNanoEngine();

private:
    MossNanoEngine() = default;
    mutable std::mutex mu_;
    void* handle_ = nullptr;
    std::string model_;
    std::string codec_;
    std::string tokenizer_;
    std::string error_;
};

} // namespace pm::tts
