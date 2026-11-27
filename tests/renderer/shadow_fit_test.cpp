// Directional shadow fit to the camera view (renderer/shadow_fit.hpp): the
// map covers the view slice, reaches casters toward the sun, and stays
// texel-aligned as the camera moves.

#include <array>
#include <cmath>

#include <glm/glm.hpp>
#include <glm/gtc/matrix_transform.hpp>
#include <gtest/gtest.h>

#include "engine/renderer/shadow_fit.hpp"

namespace {

using engine::renderer::fit_shadow_to_view;
using engine::renderer::frustum_slice_sphere;
using engine::renderer::ShadowViewFit;

constexpr float k_fov = 0.96F; // ~55 degrees
constexpr float k_aspect = 16.0F / 9.0F;

glm::mat4 camera_at(const glm::vec3& eye, const glm::vec3& target) {
    return glm::inverse(glm::lookAt(eye, target, glm::vec3(0.0F, 1.0F, 0.0F)));
}

// The apex and the four far corners of the view slice [0, distance].
std::array<glm::vec3, 5> slice_points(const glm::mat4& world, float distance) {
    const float t = std::tan(0.5F * k_fov);
    const glm::vec3 eye(world[3]);
    const glm::vec3 right(world[0]);
    const glm::vec3 up(world[1]);
    const glm::vec3 fwd = -glm::vec3(world[2]);
    std::array<glm::vec3, 5> out{eye, eye, eye, eye, eye};
    for (int i = 0; i < 4; ++i) {
        const float sx = (i & 1) != 0 ? 1.0F : -1.0F;
        const float sy = (i & 2) != 0 ? 1.0F : -1.0F;
        out[static_cast<std::size_t>(i) + 1] =
            eye + distance * (fwd + sx * t * k_aspect * right + sy * t * up);
    }
    return out;
}

glm::vec3 project(const glm::mat4& vp, const glm::vec3& p) {
    const glm::vec4 c = vp * glm::vec4(p, 1.0F);
    return glm::vec3(c) / c.w;
}

TEST(ShadowFit, SphereBoundsTheSlice) {
    for (const float aspect : {1.0F, k_aspect, 3.0F}) {
        const auto s = frustum_slice_sphere(k_fov, aspect, 100.0F);
        const float t = std::tan(0.5F * k_fov);
        const float k = t * std::sqrt(1.0F + aspect * aspect);
        EXPECT_LE(s.centre_depth, 100.0F);
        EXPECT_GE(s.radius + 1e-3F, s.centre_depth) << "apex outside";
        EXPECT_GE(s.radius + 1e-3F, std::hypot(100.0F - s.centre_depth, 100.0F * k))
            << "far corner outside";
    }
    // Narrow view: the sphere touches the apex and the far corners alike.
    const auto narrow = frustum_slice_sphere(0.3F, 1.0F, 50.0F);
    EXPECT_NEAR(narrow.radius, narrow.centre_depth, 1e-3F);
}

TEST(ShadowFit, MapCoversTheViewAndCastersTowardTheSun) {
    const glm::vec3 sun = glm::normalize(glm::vec3(0.87F, -0.16F, 0.47F));
    const ShadowViewFit fit{.distance = 160.0F, .caster_reach = 160.0F, .map_size = 2048};
    const glm::mat4 world = camera_at({-600.0F, 273.0F, -200.0F}, {460.0F, 420.0F, -1260.0F});
    const glm::mat4 vp = fit_shadow_to_view(world, k_fov, k_aspect, sun, fit);
    for (const glm::vec3& p : slice_points(world, fit.distance)) {
        const glm::vec3 ndc = project(vp, p);
        EXPECT_LE(std::abs(ndc.x), 1.0F);
        EXPECT_LE(std::abs(ndc.y), 1.0F);
        EXPECT_GT(ndc.z, 0.0F);
        EXPECT_LT(ndc.z, 1.0F);
        // A caster up to caster_reach toward the sun still renders.
        const glm::vec3 caster = project(vp, p - sun * (fit.caster_reach * 0.99F));
        EXPECT_GE(caster.z, 0.0F);
        EXPECT_NEAR(caster.x, ndc.x, 1e-3F) << "light rays are parallel";
    }
}

// Moving the camera slides the window in whole texels: a fixed world point
// keeps its sub-texel position in the map, so shadow edges do not shimmer.
TEST(ShadowFit, WindowMovesInWholeTexels) {
    const glm::vec3 sun = glm::normalize(glm::vec3(0.3F, -0.8F, 0.5F));
    const ShadowViewFit fit{.distance = 60.0F, .caster_reach = 60.0F, .map_size = 2048};
    const glm::vec3 probe(3.0F, 1.0F, -20.0F);
    const auto texel_of = [&](const glm::vec3& eye) {
        const glm::mat4 vp =
            fit_shadow_to_view(camera_at(eye, eye + glm::vec3(0.2F, -0.3F, -1.0F)), k_fov,
                               k_aspect, sun, fit);
        const glm::vec3 ndc = project(vp, probe);
        return glm::vec2(ndc) * 0.5F * static_cast<float>(fit.map_size);
    };
    const glm::vec2 base = texel_of({0.0F, 5.0F, 0.0F});
    for (const glm::vec3& eye : {glm::vec3(0.37F, 5.0F, 0.11F), glm::vec3(-1.3F, 5.2F, 2.9F),
                                 glm::vec3(4.05F, 4.4F, -3.3F)}) {
        const glm::vec2 moved = texel_of(eye) - base;
        EXPECT_NEAR(moved.x, std::round(moved.x), 0.02F);
        EXPECT_NEAR(moved.y, std::round(moved.y), 0.02F);
    }
}

} // namespace
