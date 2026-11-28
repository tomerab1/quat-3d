// FrameTimer — see engine/core/frame_timer.hpp.

#include "engine/core/frame_timer.hpp"

namespace engine::core {

float FrameTimer::tick(std::uint64_t now_ns) {
    const bool forward = started_ && now_ns > last_ns_;
    const float dt = forward ? static_cast<float>(now_ns - last_ns_) * 1e-9F : 0.0F;
    if (!started_ || now_ns > last_ns_) last_ns_ = now_ns;
    started_ = true;
    return dt;
}

void FrameTimer::skip(std::uint64_t now_ns) {
    if (!started_ || now_ns > last_ns_) last_ns_ = now_ns;
    started_ = true;
}

} // namespace engine::core
