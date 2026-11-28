// View clip range — see engine/scene/view_clip.hpp.

#include "engine/scene/view_clip.hpp"

#include <algorithm>
#include <cmath>

namespace engine::scene {

namespace {

// Distance from `eye` to the farthest corner of `box`.
[[nodiscard]] float farthest_corner(const glm::vec3& eye, const asset::Aabb& box) {
    const glm::vec3 d = glm::max(glm::abs(box.min - eye), glm::abs(box.max - eye));
    return glm::length(d);
}

[[nodiscard]] bool usable(const asset::Aabb& box) {
    return box.min.x <= box.max.x && box.min.y <= box.max.y && box.min.z <= box.max.z &&
           std::isfinite(box.min.x + box.min.y + box.min.z + box.max.x + box.max.y + box.max.z);
}

} // namespace

float near_for_far(float far_z) {
    return std::clamp(far_z / k_max_far_near_ratio, k_min_near, k_max_near);
}

ClipRange derive_clip_range(const glm::vec3& eye, const ViewExtent& extent) {
    float reach = 0.0F;
    for (const auto& box : {extent.drawables, extent.terrain}) {
        if (box && usable(*box)) reach = std::max(reach, farthest_corner(eye, *box));
    }
    // 10% margin: the scene moves between the (periodic) bounds refreshes.
    float far_z = reach * 1.1F;
    if (!std::isfinite(far_z)) far_z = k_max_far;
    far_z = std::clamp(far_z, k_min_far, k_max_far);
    return {near_for_far(far_z), far_z};
}

ClipRange apply_clip_overrides(const ClipRange& derived, std::optional<float> near_override,
                               std::optional<float> far_override) {
    ClipRange out = derived;
    if (far_override) {
        out.far_z = *far_override;
        out.near_z = std::min(near_for_far(out.far_z), out.far_z * 0.5F);
    }
    if (near_override) {
        out.near_z = *near_override;
        if (out.far_z <= out.near_z) out.far_z = out.near_z * 1000.0F;
    }
    return out;
}

asset::Aabb streaming_window_bounds(const glm::vec3& eye, const glm::vec3& anchor, float tile_size,
                                    int radius, float min_height, float max_height) {
    const float t = std::max(tile_size, 1e-3F);
    const auto r = static_cast<float>(std::max(radius, 0));
    // The streamer's camera tile: tile (0,0) spans [-t/2, t/2) around the anchor.
    const float cx = std::floor((eye.x - anchor.x) / t + 0.5F);
    const float cz = std::floor((eye.z - anchor.z) / t + 0.5F);
    return {{anchor.x + (cx - r - 0.5F) * t, anchor.y + min_height, anchor.z + (cz - r - 0.5F) * t},
            {anchor.x + (cx + r + 0.5F) * t, anchor.y + max_height,
             anchor.z + (cz + r + 0.5F) * t}};
}

} // namespace engine::scene
