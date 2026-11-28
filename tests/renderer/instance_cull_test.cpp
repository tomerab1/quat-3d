// CPU reference of the GPU instance cull (R6 / R9.4, instance_cull.hpp): the
// per-view tests the GPU kernels mirror. GpuInstanceCull.* checks the GPU
// against reference_cull.

#include <array>
#include <cstdint>
#include <vector>

#include <glm/glm.hpp>
#include <glm/gtc/matrix_transform.hpp>
#include <gtest/gtest.h>

#include "engine/renderer/csm.hpp"
#include "engine/renderer/instance_cull.hpp"

namespace {

namespace r = engine::renderer;

// A unit-radius sphere draw at the object origin.
r::CullDrawGpu unit_draw(std::uint32_t count = 1) {
    r::CullDrawGpu d;
    d.local_sphere = {0.0F, 0.0F, 0.0F, 1.0F};
    d.instance_count = count;
    d.command_count = 1;
    return d;
}

glm::mat4 camera_view_proj() {
    glm::mat4 proj = glm::perspective(glm::radians(60.0F), 1.0F, 0.1F, 100.0F);
    proj[1][1] *= -1.0F;
    return proj * glm::lookAt(glm::vec3(0.0F), glm::vec3(0.0F, 0.0F, -1.0F),
                              glm::vec3(0.0F, 1.0F, 0.0F));
}

// A sun straight overhead and its cascades fitted to camera_view_proj's view.
r::GpuCascades overhead_cascades(float caster_distance = 50.0F) {
    r::CsmView view;
    view.view = glm::lookAt(glm::vec3(0.0F), glm::vec3(0.0F, 0.0F, -1.0F),
                            glm::vec3(0.0F, 1.0F, 0.0F));
    view.projection = glm::perspective(glm::radians(60.0F), 1.0F, 0.1F, 100.0F);
    view.projection[1][1] *= -1.0F;
    view.to_light = {0.0F, 1.0F, 0.0F};
    r::CsmSettings s;
    s.max_distance = 80.0F;
    s.caster_distance = caster_distance;
    return r::fit_cascades(view, s, {0.1F, 80.0F}, 2048, true);
}

// Centre of cascade c's footprint on the ground (light space origin).
glm::vec3 cascade_centre(const r::GpuCascades& cs, std::uint32_t c) {
    const glm::mat4 inv = glm::inverse(cs.view_proj[c]);
    const glm::vec4 p = inv * glm::vec4(0.0F, 0.0F, 0.5F, 1.0F);
    return glm::vec3(p) / p.w;
}

} // namespace

TEST(InstanceCull, PlanesAreInwardAndNormalised) {
    const auto planes = r::frustum_planes(camera_view_proj());
    for (const glm::vec4& p : planes) {
        EXPECT_NEAR(glm::length(glm::vec3(p)), 1.0F, 1e-5F);
        // A point straight ahead, inside the frustum, is on the inner side.
        EXPECT_GT(glm::dot(glm::vec3(p), glm::vec3(0.0F, 0.0F, -10.0F)) + p.w, 0.0F);
    }
}

TEST(InstanceCull, CameraViewKeepsVisibleAndStraddlingSpheres) {
    const r::ResolvedCullView v =
        r::resolve_cull_view(r::camera_cull_view(camera_view_proj()), r::GpuCascades{});
    const r::CullDrawGpu d = unit_draw();
    const glm::vec3 cam(0.0F);
    EXPECT_TRUE(r::cull_accepts(v, d, {0.0F, 0.0F, -10.0F, 1.0F}, cam));  // inside
    EXPECT_FALSE(r::cull_accepts(v, d, {0.0F, 0.0F, 10.0F, 1.0F}, cam));  // behind
    EXPECT_FALSE(r::cull_accepts(v, d, {0.0F, 0.0F, -120.0F, 1.0F}, cam)); // past far
    // Centre 0.5 outside the left plane at z = -10 (half-width 10 tan 30°),
    // radius 1: straddles, kept.
    const float edge = 10.0F * std::tan(glm::radians(30.0F));
    EXPECT_TRUE(r::cull_accepts(v, d, {-edge - 0.5F, 0.0F, -10.0F, 1.0F}, cam));
    EXPECT_FALSE(r::cull_accepts(v, d, {-edge - 2.0F, 0.0F, -10.0F, 1.0F}, cam));
}

TEST(InstanceCull, WorldSphereScalesByTheLargestAxis) {
    glm::mat4 m = glm::translate(glm::mat4(1.0F), glm::vec3(1.0F, 2.0F, 3.0F));
    m = glm::scale(m, glm::vec3(1.0F, 3.0F, 2.0F));
    const glm::vec4 s = r::world_sphere(m, {0.0F, 1.0F, 0.0F, 0.5F});
    EXPECT_NEAR(s.x, 1.0F, 1e-6F);
    EXPECT_NEAR(s.y, 5.0F, 1e-6F);
    EXPECT_NEAR(s.z, 3.0F, 1e-6F);
    EXPECT_NEAR(s.w, 1.5F, 1e-6F);
}

TEST(InstanceCull, CascadeKeepsCastersTowardTheSunWithinTheReach) {
    const r::GpuCascades cs = overhead_cascades(50.0F);
    const r::ResolvedCullView v = r::resolve_cull_view(r::cascade_cull_view(0, 0.0F), cs);
    const r::CullDrawGpu d = unit_draw();
    const glm::vec3 c = cascade_centre(cs, 0);
    // Above the cascade's sphere (toward the sun) but inside the caster
    // reach: a pancaked caster that can shadow the cascade.
    const float top = c.y + 0.5F * cs.depth_range.x;
    EXPECT_TRUE(r::cull_accepts(v, d, {c.x, top + 30.0F, c.z, 1.0F}, glm::vec3(0.0F)));
    EXPECT_FALSE(r::cull_accepts(v, d, {c.x, top + 80.0F, c.z, 1.0F}, glm::vec3(0.0F)));
    // Beside the cascade's footprint: cannot shadow anything in it.
    const float half = 1024.0F * cs.texel_world.x; // half the map in world units
    EXPECT_FALSE(r::cull_accepts(v, d, {c.x + half + 2.0F, c.y, c.z, 1.0F}, glm::vec3(0.0F)));
    EXPECT_TRUE(r::cull_accepts(v, d, {c.x + 0.5F * half, c.y, c.z, 1.0F}, glm::vec3(0.0F)));
}

TEST(InstanceCull, ProjectedSizeDropsSubTexelCastersFromCoarseCascadesOnly) {
    const r::GpuCascades cs = overhead_cascades();
    ASSERT_LT(cs.texel_world.x, cs.texel_world.w);
    // A caster whose diameter is two texels of cascade 0 and well under one
    // of cascade 3.
    const float radius = cs.texel_world.x;
    ASSERT_LT(2.0F * radius, cs.texel_world.w);
    const r::CullDrawGpu d = unit_draw();
    for (std::uint32_t c : {0U, 3U}) {
        const glm::vec3 p = cascade_centre(cs, c);
        const glm::vec4 sphere(p, radius);
        const r::ResolvedCullView on = r::resolve_cull_view(r::cascade_cull_view(c, 1.0F), cs);
        const r::ResolvedCullView off = r::resolve_cull_view(r::cascade_cull_view(c, 0.0F), cs);
        EXPECT_TRUE(r::cull_accepts(off, d, sphere, glm::vec3(0.0F))) << "cascade " << c;
        EXPECT_EQ(r::cull_accepts(on, d, sphere, glm::vec3(0.0F)), c == 0) << "cascade " << c;
    }
}

TEST(InstanceCull, CasterDistanceAppliesToShadowViewsOnly) {
    const r::GpuCascades cs = overhead_cascades();
    r::CullDrawGpu d = unit_draw();
    d.shadow_distance = 10.0F;
    const glm::vec3 p = cascade_centre(cs, 3);
    const glm::vec3 near_cam = p + glm::vec3(0.0F, 0.0F, 5.0F);
    const glm::vec3 far_cam = p + glm::vec3(0.0F, 0.0F, 20.0F);
    const r::ResolvedCullView shadow = r::resolve_cull_view(r::cascade_cull_view(3, 0.0F), cs);
    EXPECT_TRUE(r::cull_accepts(shadow, d, {p, 1.0F}, near_cam));
    EXPECT_FALSE(r::cull_accepts(shadow, d, {p, 1.0F}, far_cam));
    // 11 m from the camera but the sphere's edge is at 10: still casts.
    EXPECT_TRUE(r::cull_accepts(shadow, d, {p, 1.0F}, p + glm::vec3(0.0F, 0.0F, 11.0F)));
    const r::ResolvedCullView all = r::resolve_cull_view(r::keep_all_cull_view(), cs);
    EXPECT_TRUE(r::cull_accepts(all, d, {p, 1.0F}, far_cam));
}

TEST(InstanceCull, NeverCullDrawsAndKeepAllViewsKeepEverything) {
    const r::ResolvedCullView cam =
        r::resolve_cull_view(r::camera_cull_view(camera_view_proj()), r::GpuCascades{});
    r::CullDrawGpu d = unit_draw();
    const glm::vec4 behind(0.0F, 0.0F, 50.0F, 1.0F);
    EXPECT_FALSE(r::cull_accepts(cam, d, behind, glm::vec3(0.0F)));
    d.flags = r::cull_draw_never_cull;
    EXPECT_TRUE(r::cull_accepts(cam, d, behind, glm::vec3(0.0F)));
}

TEST(InstanceCull, ReferenceListsEachViewsSurvivorsPerDrawInOrder) {
    // Two draws over one instance array: a row of instances along -z.
    std::vector<glm::mat4> models;
    for (int i = 0; i < 10; ++i) {
        models.push_back(glm::translate(glm::mat4(1.0F),
                                        glm::vec3(0.0F, 0.0F, -16.0F * static_cast<float>(i) + 5.0F)));
    }
    std::array<r::CullDrawGpu, 2> draws{unit_draw(6), unit_draw(4)};
    draws[1].first_instance = 6;
    const std::array<r::CullViewGpu, 2> views{r::camera_cull_view(camera_view_proj()),
                                              r::keep_all_cull_view()};
    const auto out = r::reference_cull(views, r::GpuCascades{}, draws, models, glm::vec3(0.0F));
    ASSERT_EQ(out.size(), 2U);
    // z = 5 (behind) and z <= -107 (past far 100 by more than the radius)
    // are culled.
    EXPECT_EQ(out[0][0], (std::vector<std::uint32_t>{1, 2, 3, 4, 5}));
    EXPECT_EQ(out[0][1], (std::vector<std::uint32_t>{6}));
    EXPECT_EQ(out[1][0], (std::vector<std::uint32_t>{0, 1, 2, 3, 4, 5}));
    EXPECT_EQ(out[1][1], (std::vector<std::uint32_t>{6, 7, 8, 9}));
}
