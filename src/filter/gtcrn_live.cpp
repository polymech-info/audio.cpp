#include "pm/audio/filter/gtcrn.hpp"
#include "pm/audio/filter/models.hpp"
#include "pm/audio/filter/onnx_run.hpp"
#include "pm/audio/log.hpp"



#include <algorithm>
#include <atomic>
#include <cmath>
#include <condition_variable>
#include <cstring>
#include <deque>
#include <mutex>
#include <thread>
#include <vector>

namespace pm::audio::filter::gtcrn {
namespace {

#if defined(PM_AUDIO_HAS_ONNX) && PM_AUDIO_HAS_ONNX

constexpr int kNfft = 512;
constexpr int kHop  = 256;
constexpr int kBins = kNfft / 2 + 1;
constexpr float kPi = 3.14159265358979323846f;

void fft_radix2(float* re, float* im, int n, bool inverse)
{
    int j = 0;
    for (int i = 1; i < n; ++i) {
        int bit = n >> 1;
        for (; j & bit; bit >>= 1)
            j ^= bit;
        j ^= bit;
        if (i < j) {
            std::swap(re[i], re[j]);
            std::swap(im[i], im[j]);
        }
    }
    for (int len = 2; len <= n; len <<= 1) {
        const float ang = (inverse ? 2.f : -2.f) * kPi / static_cast<float>(len);
        const float wlen_re = std::cos(ang);
        const float wlen_im = std::sin(ang);
        for (int i = 0; i < n; i += len) {
            float wr = 1.f, wi = 0.f;
            const int half = len / 2;
            for (int k = 0; k < half; ++k) {
                const int u = i + k;
                const int v = u + half;
                const float vr = re[v] * wr - im[v] * wi;
                const float vi = re[v] * wi + im[v] * wr;
                re[v] = re[u] - vr;
                im[v] = im[u] - vi;
                re[u] += vr;
                im[u] += vi;
                const float nwr = wr * wlen_re - wi * wlen_im;
                wi = wr * wlen_im + wi * wlen_re;
                wr = nwr;
            }
        }
    }
    if (inverse) {
        const float s = 1.f / static_cast<float>(n);
        for (int i = 0; i < n; ++i) {
            re[i] *= s;
            im[i] *= s;
        }
    }
}

void rfft(const float* in, float* spec)
{
    float re[kNfft];
    float im[kNfft];
    std::memcpy(re, in, sizeof(re));
    std::memset(im, 0, sizeof(im));
    fft_radix2(re, im, kNfft, false);
    for (int k = 0; k < kBins; ++k) {
        spec[2 * k]     = re[k];
        spec[2 * k + 1] = im[k];
    }
}

void irfft(const float* spec, float* out)
{
    float re[kNfft];
    float im[kNfft];
    re[0] = spec[0];
    im[0] = 0.f;
    for (int k = 1; k < kBins - 1; ++k) {
        re[k] = spec[2 * k];
        im[k] = spec[2 * k + 1];
        re[kNfft - k] = spec[2 * k];
        im[kNfft - k] = -spec[2 * k + 1];
    }
    re[kBins - 1] = spec[2 * (kBins - 1)];
    im[kBins - 1] = 0.f;
    fft_radix2(re, im, kNfft, true);
    std::memcpy(out, re, sizeof(re));
}

size_t numel(const std::vector<int64_t>& shape)
{
    size_t n = 1;
    for (auto d : shape)
        n *= static_cast<size_t>(d < 1 ? 1 : d);
    return n;
}

int find_name(const std::vector<onnxrt::Tensor>& info, const char* name)
{
    for (int i = 0; i < static_cast<int>(info.size()); ++i) {
        if (info[static_cast<size_t>(i)].name == name)
            return i;
    }
    return -1;
}

#endif // FEATURE_AUDIO_GTCRN && FEATURE_ONNX

} // namespace

#if defined(PM_AUDIO_HAS_ONNX) && PM_AUDIO_HAS_ONNX

struct StreamSession::Impl {
    onnxrt::Session sess;
    std::vector<float> window;
    std::vector<float> analysis;
    std::vector<float> ola;
    std::vector<float> pending;
    std::vector<float> fft_in;
    std::vector<float> mix;
    std::vector<float> enh;
    std::vector<float> conv;
    std::vector<float> tra;
    std::vector<float> inter;
    std::vector<int64_t> mix_shape{1, kBins, 1, 2};
    std::vector<int64_t> conv_shape{2, 1, 16, 16, 33};
    std::vector<int64_t> tra_shape{2, 3, 1, 1, 16};
    std::vector<int64_t> inter_shape{2, 1, 33, 16};
    int in_mix = 0, in_conv = 1, in_tra = 2, in_inter = 3;
    int out_enh = 0, out_conv = 1, out_tra = 2, out_inter = 3;
    bool started = false;

    void emit_s16(const float* hop, std::vector<int16_t>& out)
    {
        for (int i = 0; i < kHop; ++i) {
            const float x = std::clamp(hop[i], -1.0f, 1.0f);
            out.push_back(static_cast<int16_t>(x * 32767.0f));
        }
    }

    void process_hop(const float* hop, std::vector<int16_t>& out)
    {
        std::move(analysis.begin() + kHop, analysis.end(), analysis.begin());
        std::copy(hop, hop + kHop, analysis.end() - kHop);
        for (int i = 0; i < kNfft; ++i)
            fft_in[static_cast<size_t>(i)] = analysis[static_cast<size_t>(i)] * window[static_cast<size_t>(i)];
        rfft(fft_in.data(), mix.data());

        std::vector<std::vector<float>> inputs(4);
        std::vector<std::vector<int64_t>> shapes(4);
        inputs[static_cast<size_t>(in_mix)]   = mix;
        inputs[static_cast<size_t>(in_conv)]  = conv;
        inputs[static_cast<size_t>(in_tra)]   = tra;
        inputs[static_cast<size_t>(in_inter)] = inter;
        shapes[static_cast<size_t>(in_mix)]   = mix_shape;
        shapes[static_cast<size_t>(in_conv)]  = conv_shape;
        shapes[static_cast<size_t>(in_tra)]   = tra_shape;
        shapes[static_cast<size_t>(in_inter)] = inter_shape;

        auto r = sess.run(inputs, shapes);
        if (!r.ok || r.outputs_f32.size() <= static_cast<size_t>(std::max(out_enh, out_inter))) {
            emit_s16(hop, out);
            return;
        }
        const auto& enh_v = r.outputs_f32[static_cast<size_t>(out_enh)];
        if (enh_v.size() != mix.size()) {
            emit_s16(hop, out);
            return;
        }
        enh = enh_v;
        if (static_cast<size_t>(out_conv) < r.outputs_f32.size() &&
            r.outputs_f32[static_cast<size_t>(out_conv)].size() == conv.size())
            conv = r.outputs_f32[static_cast<size_t>(out_conv)];
        if (static_cast<size_t>(out_tra) < r.outputs_f32.size() &&
            r.outputs_f32[static_cast<size_t>(out_tra)].size() == tra.size())
            tra = r.outputs_f32[static_cast<size_t>(out_tra)];
        if (static_cast<size_t>(out_inter) < r.outputs_f32.size() &&
            r.outputs_f32[static_cast<size_t>(out_inter)].size() == inter.size())
            inter = r.outputs_f32[static_cast<size_t>(out_inter)];

        std::vector<float> time(static_cast<size_t>(kNfft));
        irfft(enh.data(), time.data());

        std::move(ola.begin() + kHop, ola.end(), ola.begin());
        std::fill(ola.end() - kHop, ola.end(), 0.0f);
        for (int i = 0; i < kNfft; ++i)
            ola[static_cast<size_t>(i)] += time[static_cast<size_t>(i)] * window[static_cast<size_t>(i)];

        if (!started) {
            started = true;
            return;
        }
        emit_s16(ola.data(), out);
    }
};

StreamSession::StreamSession() = default;
StreamSession::~StreamSession() { close(); }

bool StreamSession::open(const std::string& model_hint, std::string& err)
{
    close();
    if (!onnx_allowed(err))
        return false;
    const auto probe = probe_gtcrn_model(model_hint);
    if (probe.path.empty()) {
        err = gtcrn_model_not_found_message(probe);
        return false;
    }
    auto impl = std::make_unique<Impl>();
    if (!impl->sess.load(probe.path, err)) {
        if (err.empty())
            err = "gtcrn onnx load failed";
        return false;
    }
    const auto ins  = impl->sess.inputs();
    const auto outs = impl->sess.outputs();
    impl->in_mix    = find_name(ins, "mix");
    impl->in_conv   = find_name(ins, "conv_cache");
    impl->in_tra    = find_name(ins, "tra_cache");
    impl->in_inter  = find_name(ins, "inter_cache");
    impl->out_enh   = find_name(outs, "enh");
    impl->out_conv  = find_name(outs, "conv_cache_out");
    impl->out_tra   = find_name(outs, "tra_cache_out");
    impl->out_inter = find_name(outs, "inter_cache_out");
    if (impl->in_mix < 0 || impl->in_conv < 0 || impl->in_tra < 0 || impl->in_inter < 0 ||
        impl->out_enh < 0 || impl->out_conv < 0 || impl->out_tra < 0 || impl->out_inter < 0) {
        err = "gtcrn_simple.onnx is missing mix/conv_cache/tra_cache/inter_cache I/O";
        return false;
    }
    impl->window.resize(static_cast<size_t>(kNfft));
    for (int i = 0; i < kNfft; ++i) {
        const float hann = 0.5f - 0.5f * std::cos(2.f * kPi * static_cast<float>(i) /
                                                  static_cast<float>(kNfft));
        impl->window[static_cast<size_t>(i)] = std::sqrt(hann);
    }
    impl->analysis.assign(static_cast<size_t>(kNfft), 0.0f);
    impl->ola.assign(static_cast<size_t>(kNfft), 0.0f);
    impl->fft_in.assign(static_cast<size_t>(kNfft), 0.0f);
    impl->mix.assign(static_cast<size_t>(kBins * 2), 0.0f);
    impl->enh.assign(static_cast<size_t>(kBins * 2), 0.0f);
    impl->conv.assign(numel(impl->conv_shape), 0.0f);
    impl->tra.assign(numel(impl->tra_shape), 0.0f);
    impl->inter.assign(numel(impl->inter_shape), 0.0f);
    impl_ = std::move(impl);
    return true;
}

void StreamSession::close()
{
    impl_.reset();
}

bool StreamSession::is_open() const noexcept
{
    return static_cast<bool>(impl_);
}

void StreamSession::process(const int16_t* in, size_t frames, std::vector<int16_t>& out,
                             const std::atomic_bool* cancel)
{
    if (!impl_ || !in || frames == 0)
        return;
    impl_->pending.reserve(impl_->pending.size() + frames);
    for (size_t i = 0; i < frames; ++i)
        impl_->pending.push_back(static_cast<float>(in[i]) / 32768.0f);
    size_t consumed = 0;
    while (impl_->pending.size() - consumed >= static_cast<size_t>(kHop)) {
        if (cancel && cancel->load(std::memory_order_relaxed))
            break;
        impl_->process_hop(impl_->pending.data() + consumed, out);
        consumed += static_cast<size_t>(kHop);
    }
    if (consumed)
        impl_->pending.erase(impl_->pending.begin(),
                             impl_->pending.begin() + static_cast<std::ptrdiff_t>(consumed));
}

struct LiveHop::Impl {
    StreamSession sess;
    std::function<void(const int16_t*, size_t)> out;
    std::thread worker;
    std::mutex mu;
    std::condition_variable cv;
    std::deque<int16_t> q;
    std::atomic<bool> stop{false};
    std::atomic<bool> pass{false};
    std::atomic<bool> alive{false};

    static constexpr size_t kBehind = 256 * 3;
    static constexpr size_t kCap    = 16000;

    void run()
    {
        std::vector<int16_t> chunk;
        std::vector<int16_t> enhanced;
        while (!stop.load(std::memory_order_relaxed)) {
            {
                std::unique_lock<std::mutex> lk(mu);
                cv.wait(lk, [&] {
                    return stop.load(std::memory_order_relaxed) || !q.empty();
                });
                if (stop.load(std::memory_order_relaxed))
                    break;
                chunk.assign(q.begin(), q.end());
                q.clear();
            }
            if (chunk.empty())
                continue;
            if (pass.exchange(false) || !sess.is_open()) {
                if (out)
                    out(chunk.data(), chunk.size());
                continue;
            }
            enhanced.clear();
            sess.process(chunk.data(), chunk.size(), enhanced);
            if (!enhanced.empty() && out)
                out(enhanced.data(), enhanced.size());
        }
    }
};

LiveHop::LiveHop() = default;
LiveHop::~LiveHop() { stop(); }

bool LiveHop::start(std::function<void(const int16_t*, size_t)> out, std::string& err)
{
    stop();
    auto impl = std::make_unique<Impl>();
    if (!impl->sess.open({}, err))
        return false;
    impl->out = std::move(out);
    impl->alive.store(true);
    impl->worker = std::thread([p = impl.get()] { p->run(); });
    impl_ = std::move(impl);
    return true;
}

void LiveHop::enqueue(const int16_t* pcm, size_t frames)
{
    if (!impl_ || !pcm || frames == 0 || !impl_->alive.load())
        return;
    std::lock_guard<std::mutex> lk(impl_->mu);
    if (impl_->q.size() + frames > Impl::kCap) {
        const size_t drop = impl_->q.size() + frames - Impl::kCap;
        impl_->q.erase(impl_->q.begin(),
                       impl_->q.begin() + static_cast<std::ptrdiff_t>(
                           std::min(drop, impl_->q.size())));
        impl_->pass.store(true);
    }
    impl_->q.insert(impl_->q.end(), pcm, pcm + frames);
    if (impl_->q.size() > Impl::kBehind)
        impl_->pass.store(true);
    impl_->cv.notify_one();
}

void LiveHop::stop()
{
    if (!impl_)
        return;
    impl_->stop.store(true);
    impl_->cv.notify_all();
    if (impl_->worker.joinable())
        impl_->worker.join();
    impl_->alive.store(false);
    impl_->sess.close();
    impl_.reset();
}

bool LiveHop::running() const noexcept
{
    return impl_ && impl_->alive.load();
}

#else

struct StreamSession::Impl {};
struct LiveHop::Impl {};

StreamSession::StreamSession() = default;
StreamSession::~StreamSession() = default;
bool StreamSession::open(const std::string&, std::string& err)
{
        err = "GTCRN needs the ONNX runtime pointed at Tanit";
    return false;
}
void StreamSession::close() {}
bool StreamSession::is_open() const noexcept { return false; }
void StreamSession::process(const int16_t*, size_t, std::vector<int16_t>&,
                             const std::atomic_bool*) {}

LiveHop::LiveHop() = default;
LiveHop::~LiveHop() = default;
bool LiveHop::start(std::function<void(const int16_t*, size_t)>, std::string& err)
{
        err = "GTCRN needs the ONNX runtime pointed at Tanit";
    return false;
}
void LiveHop::enqueue(const int16_t*, size_t) {}
void LiveHop::stop() {}
bool LiveHop::running() const noexcept { return false; }

#endif

} // namespace pm::audio::filter::gtcrn

