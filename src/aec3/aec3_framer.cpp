#include "aec3_framer.hpp"

#include <cstring>

namespace pm::audio::aec3 {

FrameBuffer::FrameBuffer(std::size_t frame_size)
    : frame_size_(frame_size)
{
}

void FrameBuffer::push(const std::int16_t* pcm, std::size_t count)
{
    if (!pcm || count == 0 || frame_size_ == 0)
        return;
    pending_.insert(pending_.end(), pcm, pcm + count);
}

bool FrameBuffer::pop(std::int16_t* dst)
{
    if (!dst || frame_size_ == 0 || pending_.size() < frame_size_)
        return false;
    std::memcpy(dst, pending_.data(), frame_size_ * sizeof(std::int16_t));
    pending_.erase(pending_.begin(), pending_.begin() + static_cast<std::ptrdiff_t>(frame_size_));
    return true;
}

std::size_t FrameBuffer::remainder() const
{
    return pending_.size();
}

std::size_t FrameBuffer::frame_size() const
{
    return frame_size_;
}

} // namespace pm::audio::aec3
