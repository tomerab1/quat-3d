#pragma once

// FrameTimer — the frame loop's real-time delta (issue #61).
//
// tick(now) returns the seconds since the previous tick (0 on the first one,
// or when the clock did not move forward). Time the loop spends not running
// frames — a minimised window, a skipped swapchain acquire — is not frame
// time: the loop calls skip(now) on such iterations, so the first frame
// afterwards measures from the end of the gap instead of handing the whole
// gap to the camera, the renderer and the simulation as one delta.
//
// It does not clamp: long rendered frames (hitches) are bounded by the
// simulation clock (SimClock) and the physics accumulator.

#include <cstdint>

namespace engine::core {

class FrameTimer {
public:
    // Seconds since the previous tick() or skip(); records `now_ns` as the
    // new reference. 0 when there is no previous reference.
    [[nodiscard]] float tick(std::uint64_t now_ns);
    // An iteration that produced no frame: the time up to `now_ns` is dropped.
    void skip(std::uint64_t now_ns);

private:
    std::uint64_t last_ns_ = 0;
    bool          started_ = false;
};

} // namespace engine::core
