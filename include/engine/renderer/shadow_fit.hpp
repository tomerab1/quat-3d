#pragma once

// Directional shadow fit to the camera view: the single shadow map covers
// the bounding sphere of the view frustum's first `distance` metres, the
// stable fit of cascaded shadow maps (one cascade until R12). The window's
// size depends only on the frustum shape and distance (not the camera's
// orientation) and its centre snaps to whole shadow texels in light space,
// so moving or turning the camera never makes shadow edges shimmer.

#include <cstdint>

#include <glm/glm.hpp>

namespace engine::renderer {

struct ShadowViewFit {
    float         distance = 150.0F;   // metres of the view the map covers
    float         caster_reach = 100.0F; // metres toward the sun for casters outside it
    std::uint32_t map_size = 2048;     // shadow map texels per side
};

// Centre and radius of the smallest sphere around the camera frustum slice
// [0, distance] (symmetric perspective with vertical fov `fov_y`, `aspect`
// = width / height), as a distance along the view axis.
struct FrustumSphere {
    float centre_depth = 0.0F;
    float radius = 0.0F;
};
[[nodiscard]] FrustumSphere frustum_slice_sphere(float fov_y, float aspect, float distance);

// The light view-projection (orthographic, Vulkan [0, 1] depth) for a sun
// whose light travels along `sun_travel`, covering the slice of the view
// with inverse view matrix `camera_world`.
[[nodiscard]] glm::mat4 fit_shadow_to_view(const glm::mat4& camera_world, float fov_y,
                                           float aspect, const glm::vec3& sun_travel,
                                           const ShadowViewFit& fit);

} // namespace engine::renderer
