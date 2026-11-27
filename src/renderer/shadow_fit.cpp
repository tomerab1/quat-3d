// Directional shadow fit to the camera view — see engine/renderer/shadow_fit.hpp.

#include "engine/renderer/shadow_fit.hpp"

#include <algorithm>
#include <cmath>

#include <glm/gtc/matrix_transform.hpp>

namespace engine::renderer {

FrustumSphere frustum_slice_sphere(float fov_y, float aspect, float distance) {
    // A far corner sits k * depth off the axis.
    const float t = std::tan(0.5F * fov_y);
    const float k2 = t * t * (1.0F + aspect * aspect);
    const float d = std::max(distance, 0.0F);
    // Equidistant from the apex and the far corners; beyond the far plane the
    // far rectangle alone bounds the slice.
    const float centre = 0.5F * d * (1.0F + k2);
    if (centre <= d) return {centre, centre};
    return {d, d * std::sqrt(k2)};
}

ShadowViewFitResult fit_shadow_to_view(const glm::mat4& camera_world, float fov_y, float aspect,
                                       const glm::vec3& sun_travel, const ShadowViewFit& fit) {
    const glm::vec3 eye(camera_world[3]);
    const glm::vec3 forward = -glm::normalize(glm::vec3(camera_world[2]));
    const FrustumSphere sphere = frustum_slice_sphere(fov_y, aspect, fit.distance);
    const glm::vec3 centre = eye + forward * sphere.centre_depth;
    const float r = std::max(sphere.radius, 0.01F);
    const float reach = std::max(fit.caster_reach, 0.0F);

    const float len = glm::length(sun_travel);
    const glm::vec3 dir = len > 1e-6F ? sun_travel / len : glm::vec3(0.0F, -1.0F, 0.0F);
    const glm::vec3 up = std::abs(dir.y) > 0.99F ? glm::vec3(0.0F, 0.0F, 1.0F)
                                                 : glm::vec3(0.0F, 1.0F, 0.0F);
    const glm::mat4 rotation = glm::lookAt(glm::vec3(0.0F), dir, up);

    // Snap the window centre to whole texels of the light-space grid.
    glm::vec3 c = glm::vec3(rotation * glm::vec4(centre, 1.0F));
    const float texel = 2.0F * r / static_cast<float>(std::max(fit.map_size, 1U));
    c.x = std::floor(c.x / texel + 0.5F) * texel;
    c.y = std::floor(c.y / texel + 0.5F) * texel;

    // The light "eye" sits r + reach before the centre along the light, so
    // casters up to `reach` outside the sphere still render into the map.
    const float depth_range = 2.0F * r + reach;
    const glm::mat4 view =
        glm::translate(glm::mat4(1.0F), glm::vec3(-c.x, -c.y, -(c.z + r + reach))) * rotation;
    const glm::mat4 proj = glm::ortho(-r, r, -r, r, 0.0F, depth_range);

    ShadowViewFitResult out;
    out.view_proj = proj * view;
    out.depth_range_m = depth_range;
    out.texel_m = texel;
    out.depth_bias = fit.bias_texels * texel / depth_range;
    out.slope_bias = fit.slope_bias_texels * texel / depth_range;
    return out;
}

} // namespace engine::renderer
