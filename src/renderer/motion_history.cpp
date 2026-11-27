#include "engine/renderer/motion_history.hpp"

#include <utility>

namespace engine::renderer {

void MotionHistory::begin_frame() {
    // Swap rather than move so both maps keep their buckets (no per-frame
    // rehash in steady state). unordered_map nodes never move, so spans into
    // the palettes stay valid until the next swap.
    std::swap(previous_, current_);
    current_.clear();
    std::swap(previous_palettes_, current_palettes_);
    current_palettes_.clear();
}

void MotionHistory::reset() {
    previous_.clear();
    current_.clear();
    previous_palettes_.clear();
    current_palettes_.clear();
}

glm::mat4 MotionHistory::advance(std::uint64_t key, const glm::mat4& current) {
    current_[key] = current;
    const auto it = previous_.find(key);
    return it != previous_.end() ? it->second : current;
}

std::span<const glm::mat4> MotionHistory::advance_palette(std::uint64_t key,
                                                          std::span<const glm::mat4> current) {
    std::vector<glm::mat4>& slot = current_palettes_[key];
    slot.assign(current.begin(), current.end());
    const auto it = previous_palettes_.find(key);
    if (it == previous_palettes_.end() || it->second.size() != current.size()) return slot;
    return it->second;
}

} // namespace engine::renderer
