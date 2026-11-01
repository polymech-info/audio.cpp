#pragma once

#include <cstdint>
#include <memory>
#include <string>
#include <vector>

namespace pm::audio::filter::onnxrt {

struct Tensor {
    std::string name;
};

struct Run {
    bool ok = false;
    std::string error;
    std::vector<std::vector<float>> outputs_f32;
};

class Session {
public:
    Session();
    ~Session();
    Session(const Session&) = delete;
    Session& operator=(const Session&) = delete;

    bool load(const std::string& model_path, std::string& err);
    void close();
    std::vector<Tensor> inputs() const;
    std::vector<Tensor> outputs() const;
    Run run(const std::vector<std::vector<float>>& inputs,
            const std::vector<std::vector<int64_t>>& shapes);

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

} // namespace pm::audio::filter::onnxrt
