// View camera clip planes derived from the scene (issue #37).

#include <glm/glm.hpp>
#include <glm/gtc/matrix_transform.hpp>
#include <gtest/gtest.h>

#include "engine/scene/view_clip.hpp"

namespace {

namespace s = engine::scene;
using engine::asset::Aabb;

// Depth of a point `distance` metres straight ahead, as the projection writes it.
float depth_at(const s::ClipRange& clip, float distance) {
    glm::mat4 proj = glm::perspective(glm::radians(60.0F), 16.0F / 9.0F, clip.near_z, clip.far_z);
    proj[1][1] *= -1.0F;
    const glm::vec4 c = proj * glm::vec4(0.0F, 0.0F, -distance, 1.0F);
    return c.z / c.w;
}

TEST(ViewClip, EmptySceneKeepsTheDefaultPlanes) {
    const s::ClipRange clip = s::derive_clip_range(glm::vec3(0.0F), {});
    EXPECT_FLOAT_EQ(clip.near_z, s::k_min_near);
    EXPECT_FLOAT_EQ(clip.far_z, s::k_min_far);
}

TEST(ViewClip, FarPlaneReachesStreamedTerrain) {
    // 3 km tiles, stream radius 2 (the scene in issue #37), camera in a valley.
    const glm::vec3 eye(120.0F, 40.0F, -60.0F);
    const Aabb window = s::streaming_window_bounds(eye, glm::vec3(0.0F), 3000.0F, 2, -20.0F, 700.0F);
    EXPECT_FLOAT_EQ(window.min.x, -7500.0F);
    EXPECT_FLOAT_EQ(window.max.z, 7500.0F);
    EXPECT_FLOAT_EQ(window.max.y, 700.0F);

    const s::ClipRange clip = s::derive_clip_range(eye, {.drawables = std::nullopt, .terrain = window});
    const float corner = glm::length(glm::vec3(7620.0F, 660.0F, 7560.0F)); // farthest corner
    EXPECT_GE(clip.far_z, corner);
    EXPECT_LT(clip.far_z, corner * 1.2F);
    // The far/near ratio stays bounded for the D32F depth buffer.
    EXPECT_LE(clip.far_z / clip.near_z, s::k_max_far_near_ratio * 1.0001F);
    EXPECT_GE(clip.near_z, s::k_min_near);
    EXPECT_LE(clip.near_z, 0.25F) << "close-ups stay possible";

    // Terrain 10 km away lands inside the depth range, in front of the far plane.
    const float d = depth_at(clip, 10'000.0F);
    EXPECT_GT(d, 0.0F);
    EXPECT_LT(d, 1.0F);
    // ...and stays distinguishable from terrain 200 m behind it.
    EXPECT_LT(d, depth_at(clip, 10'200.0F));
}

TEST(ViewClip, WindowFollowsTheCameraTile) {
    // Tile (0,0) spans [-t/2, t/2) around the anchor; the camera at x = 1600
    // with 2 km tiles is in tile 1.
    const Aabb w = s::streaming_window_bounds(glm::vec3(1600.0F, 0.0F, 0.0F),
                                              glm::vec3(100.0F, 5.0F, 0.0F), 2000.0F, 1, 0.0F,
                                              50.0F);
    EXPECT_FLOAT_EQ(w.min.x, 100.0F - 1000.0F);
    EXPECT_FLOAT_EQ(w.max.x, 100.0F + 5000.0F);
    EXPECT_FLOAT_EQ(w.min.y, 5.0F);
    EXPECT_FLOAT_EQ(w.max.y, 55.0F);
}

TEST(ViewClip, DrawablesAndClamping) {
    const Aabb near_box{{-5.0F, 0.0F, -5.0F}, {5.0F, 3.0F, 5.0F}};
    s::ClipRange clip = s::derive_clip_range(glm::vec3(0.0F, 2.0F, 10.0F), {.drawables = near_box, .terrain = std::nullopt});
    EXPECT_FLOAT_EQ(clip.far_z, s::k_min_far) << "small scenes keep a generous minimum";

    const Aabb huge{{-1e7F, 0.0F, -1e7F}, {1e7F, 1.0F, 1e7F}};
    clip = s::derive_clip_range(glm::vec3(0.0F), {.drawables = huge, .terrain = std::nullopt});
    EXPECT_FLOAT_EQ(clip.far_z, s::k_max_far);
    EXPECT_FLOAT_EQ(clip.near_z, s::k_max_near);
}

TEST(ViewClip, OverridesReplaceEitherPlane) {
    const s::ClipRange derived{0.2F, 10'000.0F};
    s::ClipRange c = s::apply_clip_overrides(derived, std::nullopt, std::nullopt);
    EXPECT_FLOAT_EQ(c.near_z, 0.2F);
    EXPECT_FLOAT_EQ(c.far_z, 10'000.0F);

    c = s::apply_clip_overrides(derived, 0.01F, std::nullopt);
    EXPECT_FLOAT_EQ(c.near_z, 0.01F);
    EXPECT_FLOAT_EQ(c.far_z, 10'000.0F);

    // A far override pairs with the near plane the derivation would use.
    c = s::apply_clip_overrides(derived, std::nullopt, 50.0F);
    EXPECT_FLOAT_EQ(c.far_z, 50.0F);
    EXPECT_FLOAT_EQ(c.near_z, s::k_min_near);

    // An explicit near beyond the far plane pushes it out.
    c = s::apply_clip_overrides(derived, 20'000.0F, std::nullopt);
    EXPECT_GT(c.far_z, c.near_z);
}

} // namespace
