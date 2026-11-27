#pragma once

// Directional shadow fit to the camera view: the single shadow map covers
// the bounding sphere of the view frustum's first `distance` metres, the
// stable fit of cascaded shadow maps (one cascade until R12, which replaces
// this with CSM's max_distance). The window's size depends only on the
// frustum shape and distance (not the camera's orientation) and its centre
// snaps to whole shadow texels in light space, so moving or turning the
// camera never makes shadow edges shimmer.
//
// Depth bias: a fixed bias in the map's [0, 1] depth units means a world
// distance proportional to the light's depth range, which here spans
// hundreds of metres. The fit therefore returns biases sized in shadow
// texels of world distance, the scale acne actually has.

#include <cstdint>

#include <glm/glm.hpp>

namespace engine::renderer {

struct ShadowViewFit {
    float         distance = 150.0F; // metres of the view the map covers
    // Metres toward the sun beyond the covered sphere from which casters still
    // render. Trees are at most ~20 m tall: under a 10-degree sun their
    // shadows reach ~115 m.
    float         caster_reach = 100.0F;
    std::uint32_t map_size = 2048;   // shadow map texels per side
    float         bias_texels = 1.5F;       // constant bias, texels of world distance
    float         slope_bias_texels = 2.0F; // extra bias at grazing light, x (1 - N.L)
};

// Centre and radius of the smallest sphere around the camera frustum slice
// [0, distance] (symmetric perspective with vertical fov `fov_y`, `aspect`
// = width / height), as a distance along the view axis.
struct FrustumSphere {
    float centre_depth = 0.0F;
    float radius = 0.0F;
};
[[nodiscard]] FrustumSphere frustum_slice_sphere(float fov_y, float aspect, float distance);

struct ShadowViewFitResult {
    glm::mat4 view_proj{1.0F}; // orthographic, Vulkan [0, 1] depth
    float     depth_range_m = 0.0F; // world metres the [0, 1] depth spans
    float     texel_m = 0.0F;       // world size of one shadow texel
    float     depth_bias = 0.0F;    // bias_texels x texel_m, in depth units
    float     slope_bias = 0.0F;    // slope_bias_texels x texel_m, in depth units
};

// The light view-projection for a sun whose light travels along
// `sun_travel`, covering the slice of the view with inverse view matrix
// `camera_world`, and the matching depth biases.
[[nodiscard]] ShadowViewFitResult fit_shadow_to_view(const glm::mat4& camera_world, float fov_y,
                                                     float aspect, const glm::vec3& sun_travel,
                                                     const ShadowViewFit& fit);

} // namespace engine::renderer
