#pragma once

#include <cstddef>
#include <cstdint>
#include <vector>

namespace pm::audio::aec3 {

class FrameBuffer {
public:
    explicit FrameBuffer(std::size_t frame_size);

    void push(const std::int16_t* pcm, std::size_t count);
    bool pop(std::int16_t* dst);
    std::size_t remainder() const;
    std::size_t frame_size() const;

private:
    std::size_t frame_size_ = 0;
    std::vector<std::int16_t> pending_;
};

} // namespace pm::audio::aec3
