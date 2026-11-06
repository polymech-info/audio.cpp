#include "pm/audio/filter/onnx_run.hpp"

#include "pm/audio/dynlib.hpp"
#include "pm/audio/path.hpp"
#include "pm/audio/text.hpp"

#include <mutex>
#include <numeric>
#include <utility>

#if defined(PM_AUDIO_HAS_ONNX) && PM_AUDIO_HAS_ONNX

#if defined(_WIN32)
#  ifndef WIN32_LEAN_AND_MEAN
#    define WIN32_LEAN_AND_MEAN
#  endif
#  ifndef NOMINMAX
#    define NOMINMAX
#  endif
#  ifndef ORT_API_MANUAL_INIT
#    define ORT_API_MANUAL_INIT
#  endif
#  include <windows.h>
#endif

#include <onnxruntime_cxx_api.h>

namespace pm::audio::filter::onnxrt {
namespace {

bool load_ort() noexcept
{
#if defined(_WIN32)
    static std::once_flag once;
    static bool ok = false;
    std::call_once(once, [] {
        auto try_load = [](const std::string& dir) -> void* {
            if (dir.empty())
                return pm::audio::dynlib::load("onnxruntime.dll");
            return pm::audio::dynlib::load("onnxruntime.dll", dir);
        };
        void* module = try_load(pm::audio::path_to_utf8(exe_directory()));
#if defined(PM_AUDIO_ONNX_ROOT)
        if (!module)
            module = try_load(std::string(PM_AUDIO_ONNX_ROOT) + "/lib");
#endif
        if (!module)
            module = try_load({});
        if (!module)
            return;
        using GetApiBase = const OrtApiBase*(ORT_API_CALL*)();
        auto* get_base = reinterpret_cast<GetApiBase>(pm::audio::dynlib::sym(module, "OrtGetApiBase"));
        if (!get_base)
            return;
        const OrtApiBase* base = get_base();
        if (!base)
            return;
        Ort::InitApi(base->GetApi(ORT_API_VERSION));
        ok = true;
    });
    return ok;
#else
    return true;
#endif
}

} // namespace

struct Session::Impl {
    std::unique_ptr<Ort::Env> env;
    std::unique_ptr<Ort::Session> session;
    std::vector<Tensor> in_info;
    std::vector<Tensor> out_info;

    bool load(const std::string& model_path, std::string& err)
    {
        in_info.clear();
        out_info.clear();
        session.reset();
        if (!load_ort()) {
            err = "onnxruntime.dll not found";
            return false;
        }
        if (!env)
            env = std::make_unique<Ort::Env>(ORT_LOGGING_LEVEL_WARNING, "pm-audio");
        try {
            Ort::SessionOptions opts;
            opts.SetIntraOpNumThreads(1);
            opts.SetGraphOptimizationLevel(GraphOptimizationLevel::ORT_ENABLE_ALL);
#if defined(_WIN32)
            const std::filesystem::path wide = pm::audio::path_from_utf8(model_path);
            session = std::make_unique<Ort::Session>(*env, wide.wstring().c_str(), opts);
#else
            session = std::make_unique<Ort::Session>(*env, model_path.c_str(), opts);
#endif
            Ort::AllocatorWithDefaultOptions alloc;
            const size_t n_in = session->GetInputCount();
            for (size_t i = 0; i < n_in; ++i)
                in_info.push_back(Tensor{std::string(session->GetInputNameAllocated(i, alloc).get())});
            const size_t n_out = session->GetOutputCount();
            for (size_t i = 0; i < n_out; ++i)
                out_info.push_back(Tensor{std::string(session->GetOutputNameAllocated(i, alloc).get())});
            return true;
        } catch (const Ort::Exception& ex) {
            err = ex.what();
        } catch (const std::exception& ex) {
            err = ex.what();
        }
        return false;
    }

    Run run(const std::vector<std::vector<float>>& user_inputs,
            const std::vector<std::vector<int64_t>>& user_shapes)
    {
        Run result;
        if (!session) {
            result.error = "model not loaded";
            return result;
        }
        try {
            Ort::MemoryInfo mem = Ort::MemoryInfo::CreateCpu(OrtArenaAllocator, OrtMemTypeDefault);
            std::vector<Ort::Value> tensors;
            std::vector<std::vector<float>> owned;
            for (size_t i = 0; i < in_info.size(); ++i) {
                std::vector<int64_t> shape = i < user_shapes.size() ? user_shapes[i] : std::vector<int64_t>{};
                const size_t total = static_cast<size_t>(std::accumulate(
                    shape.begin(), shape.end(), int64_t{1}, [](int64_t a, int64_t b) { return a * b; }));
                std::vector<float> data = i < user_inputs.size() ? user_inputs[i] : std::vector<float>{};
                data.resize(total, 0.f);
                owned.push_back(std::move(data));
                tensors.push_back(Ort::Value::CreateTensor<float>(
                    mem, owned.back().data(), owned.back().size(), shape.data(), shape.size()));
            }
            std::vector<const char*> in_names;
            std::vector<const char*> out_names;
            for (const Tensor& t : in_info)
                in_names.push_back(t.name.c_str());
            for (const Tensor& t : out_info)
                out_names.push_back(t.name.c_str());
            auto outputs = session->Run(Ort::RunOptions{nullptr},
                in_names.data(), tensors.data(), in_names.size(),
                out_names.data(), out_names.size());
            result.ok = true;
            for (const auto& value : outputs) {
                if (!value.IsTensor()) {
                    result.outputs_f32.emplace_back();
                    continue;
                }
                const float* data = value.GetTensorData<float>();
                const size_t n = value.GetTensorTypeAndShapeInfo().GetElementCount();
                result.outputs_f32.emplace_back(data, data + n);
            }
        } catch (const Ort::Exception& ex) {
            result.error = ex.what();
        } catch (const std::exception& ex) {
            result.error = ex.what();
        }
        return result;
    }
};

Session::Session() : impl_(std::make_unique<Impl>()) {}
Session::~Session() = default;

bool Session::load(const std::string& model_path, std::string& err)
{
    return impl_->load(model_path, err);
}

void Session::close()
{
    impl_->session.reset();
    impl_->in_info.clear();
    impl_->out_info.clear();
}

std::vector<Tensor> Session::inputs() const { return impl_->in_info; }
std::vector<Tensor> Session::outputs() const { return impl_->out_info; }

Run Session::run(const std::vector<std::vector<float>>& inputs,
                 const std::vector<std::vector<int64_t>>& shapes)
{
    return impl_->run(inputs, shapes);
}

} // namespace pm::audio::filter::onnxrt

#else

namespace pm::audio::filter::onnxrt {

struct Session::Impl {};

Session::Session() : impl_(std::make_unique<Impl>()) {}
Session::~Session() = default;
bool Session::load(const std::string&, std::string& err)
{
    err = "ONNX runtime was not pointed at this build";
    return false;
}
void Session::close() {}
std::vector<Tensor> Session::inputs() const { return {}; }
std::vector<Tensor> Session::outputs() const { return {}; }
Run Session::run(const std::vector<std::vector<float>>&, const std::vector<std::vector<int64_t>>&)
{
    Run result;
    result.error = "ONNX runtime was not pointed at this build";
    return result;
}

} // namespace pm::audio::filter::onnxrt

#endif
