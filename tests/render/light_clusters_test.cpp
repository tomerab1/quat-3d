// Clustered light culling — CPU reference (R10.1). The GPU cull pass is
// checked against assign_lights_to_clusters in light_cull_test.cpp.

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <random>
#include <vector>

#include <glm/glm.hpp>
#include <glm/gtc/matrix_transform.hpp>
#include <gtest/gtest.h>

#include "engine/renderer/light_clusters.hpp"
#include "support/expect_ok.hpp"

namespace {

namespace r = engine::renderer;

constexpr float k_near = 0.1F;
constexpr float k_far = 200.0F;

// The engine's camera projection: [0,1] depth with the Vulkan Y flip.
glm::mat4 vulkan_perspective(float fov_y, float aspect, float near_z, float far_z) {
    glm::mat4 p = glm::perspective(fov_y, aspect, near_z, far_z);
    p[1][1] *= -1.0F;
    return p;
}

r::ClusterParams test_params(const glm::mat4& view = glm::mat4(1.0F)) {
    auto p = r::make_cluster_params(view, vulkan_perspective(glm::radians(60.0F), 16.0F / 9.0F,
                                                             k_near, k_far),
                                    0);
    EXPECT_TRUE(p.has_value());
    return p.value_or(r::ClusterParams{});
}

// View-space point inside cluster `c` at fractional position `f` in [0,1]^3
// (tile x/y in NDC, depth between the slice boundaries).
glm::vec3 point_in_cluster(const r::ClusterParams& p, const glm::uvec3& c, const glm::vec3& f) {
    const glm::vec2 ndc =
        (glm::vec2(c.x, c.y) + glm::vec2(f.x, f.y)) / glm::vec2(p.dims.x, p.dims.y) * 2.0F - 1.0F;
    const float d0 = r::slice_boundary(p, c.z);
    const float d1 = r::slice_boundary(p, c.z + 1U);
    const float depth = d0 + (d1 - d0) * f.z;
    const glm::vec4 h = p.inv_proj * glm::vec4(ndc, 0.5F, 1.0F);
    const glm::vec3 ray = glm::vec3(h) / h.w;
    return ray / -ray.z * depth;
}

std::uint32_t assigned_clusters(const std::vector<std::vector<std::uint32_t>>& lists) {
    return static_cast<std::uint32_t>(
        std::count_if(lists.begin(), lists.end(), [](const auto& l) { return !l.empty(); }));
}

TEST(LightClusters, RecoversNearAndFarFromTheProjection) {
    const glm::mat4 proj = vulkan_perspective(glm::radians(70.0F), 1.5F, 0.25F, 500.0F);
    auto p = r::make_cluster_params(glm::mat4(1.0F), proj, 7);
    ASSERT_OK(p);
    EXPECT_NEAR(p->depth.x, 0.25F, 1e-4F);
    EXPECT_NEAR(p->depth.y, 500.0F, 0.5F);
    EXPECT_EQ(p->dims.w, 7U);

    // TAA jitter (a clip-space x/y translation) changes neither.
    glm::mat4 jitter(1.0F);
    jitter[3][0] = 0.003F;
    jitter[3][1] = -0.002F;
    auto pj = r::make_cluster_params(glm::mat4(1.0F), jitter * proj, 0);
    ASSERT_OK(pj);
    EXPECT_NEAR(pj->depth.x, 0.25F, 1e-4F);
    EXPECT_NEAR(pj->depth.y, 500.0F, 0.5F);

    // Reversed Z ([0,1] with near -> 1) yields the same range.
    auto pr = r::make_cluster_params(glm::mat4(1.0F),
                                     vulkan_perspective(glm::radians(70.0F), 1.5F, 500.0F, 0.25F),
                                     0);
    ASSERT_OK(pr);
    EXPECT_NEAR(pr->depth.x, 0.25F, 1e-4F);
    EXPECT_NEAR(pr->depth.y, 500.0F, 0.5F);

    // The light count is clamped to the buffer size.
    auto many = r::make_cluster_params(glm::mat4(1.0F), proj, r::max_lights + 100);
    ASSERT_OK(many);
    EXPECT_EQ(many->dims.w, r::max_lights);
}

TEST(LightClusters, RejectsOrthographicProjections) {
    ASSERT_ERR(r::make_cluster_params(glm::mat4(1.0F),
                                      glm::ortho(-1.0F, 1.0F, -1.0F, 1.0F, 0.1F, 10.0F), 0));
    ASSERT_ERR(r::make_cluster_params(glm::mat4(1.0F), glm::mat4(1.0F), 0));
}

TEST(LightClusters, DepthSlicesAreExponential) {
    const r::ClusterParams p = test_params();
    EXPECT_NEAR(r::slice_boundary(p, 0), k_near, 1e-5F);
    // Far is recovered from the projection with float cancellation (P22 + 1),
    // so it is exact only to ~1e-4 relative — the cull and shading passes use
    // the same recovered value, which is what matters.
    EXPECT_NEAR(r::slice_boundary(p, r::cluster_grid_z), k_far, k_far * 1e-3F);
    // Every slice is the same factor deeper than the one before.
    const float ratio = std::pow(k_far / k_near, 1.0F / static_cast<float>(r::cluster_grid_z));
    for (std::uint32_t k = 0; k < r::cluster_grid_z; ++k) {
        const float d0 = r::slice_boundary(p, k);
        const float d1 = r::slice_boundary(p, k + 1U);
        EXPECT_NEAR(d1 / d0, ratio, 1e-3F) << "slice " << k;
        // A depth inside the slice (geometric midpoint) maps back to it.
        EXPECT_EQ(r::depth_slice(p, std::sqrt(d0 * d1)), k);
    }
    // Outside [near, far] clamps to the end slices.
    EXPECT_EQ(r::depth_slice(p, 0.01F), 0U);
    EXPECT_EQ(r::depth_slice(p, -3.0F), 0U);
    EXPECT_EQ(r::depth_slice(p, 1e6F), r::cluster_grid_z - 1U);
}

TEST(LightClusters, ViewPositionsMapToTheirTile) {
    const r::ClusterParams p = test_params();
    // Straight ahead: the centre of NDC is the boundary x = 8 / y = 4.5.
    const glm::uvec3 centre = r::cluster_of_view_position(p, glm::vec3(0.0F, 0.0F, -10.0F));
    EXPECT_EQ(centre.x, r::cluster_grid_x / 2);
    EXPECT_EQ(centre.y, r::cluster_grid_y / 2);
    EXPECT_EQ(centre.z, r::depth_slice(p, 10.0F));

    // Points sampled inside a froxel map back to it and lie in its AABB.
    std::mt19937 rng(7);
    std::uniform_real_distribution<float> f(0.05F, 0.95F);
    for (const glm::uvec3 c : {glm::uvec3(0, 0, 0), glm::uvec3(15, 8, 23), glm::uvec3(3, 7, 11),
                               glm::uvec3(8, 4, 5), glm::uvec3(15, 0, 17)}) {
        const r::ClusterAabb box = r::cluster_aabb(p, c);
        for (int i = 0; i < 16; ++i) {
            const glm::vec3 v = point_in_cluster(p, c, {f(rng), f(rng), f(rng)});
            const glm::uvec3 got = r::cluster_of_view_position(p, v);
            EXPECT_EQ(got, c) << "sample " << i;
            EXPECT_LE(r::aabb_distance_sq(box, v), 1e-8F);
        }
    }
}

// A small light in the top-left corner of the view touches only the
// clusters around that corner (the AABBs are conservative, so immediate
// neighbours may be included) — never the rest of the screen.
TEST(LightClusters, CornerLightTouchesOnlyCornerClusters) {
    const r::ClusterParams p = test_params();
    const glm::uvec3 home(0, 0, 12);
    const glm::vec3 centre = point_in_cluster(p, home, glm::vec3(0.5F));
    const float d0 = r::slice_boundary(p, home.z);
    const float d1 = r::slice_boundary(p, home.z + 1U);
    const float radius = 0.2F * (d1 - d0);
    const r::GpuLight light = r::make_point_light(centre, radius, glm::vec3(1.0F), 1.0F);

    const auto lists = r::assign_lights_to_clusters(p, {&light, 1});
    EXPECT_EQ(lists[r::cluster_index(home)], std::vector<std::uint32_t>{0});
    for (std::uint32_t z = 0; z < r::cluster_grid_z; ++z) {
        for (std::uint32_t y = 0; y < r::cluster_grid_y; ++y) {
            for (std::uint32_t x = 0; x < r::cluster_grid_x; ++x) {
                if (lists[r::cluster_index({x, y, z})].empty()) continue;
                EXPECT_LE(x, 1U);
                EXPECT_LE(y, 1U);
                EXPECT_LE(std::abs(static_cast<int>(z) - static_cast<int>(home.z)), 1);
            }
        }
    }
    EXPECT_LE(assigned_clusters(lists), 8U);
}

TEST(LightClusters, LightBehindTheCameraTouchesNothing) {
    const r::ClusterParams p = test_params();
    // Entirely behind the camera (view +Z): no cluster.
    const r::GpuLight behind = r::make_point_light({0.0F, 0.0F, 5.0F}, 3.0F, glm::vec3(1.0F), 1.0F);
    EXPECT_EQ(assigned_clusters(r::assign_lights_to_clusters(p, {&behind, 1})), 0U);

    // Behind, but its sphere reaches past the near plane: only the first
    // slices it actually reaches.
    const r::GpuLight reaching =
        r::make_point_light({0.0F, 0.0F, 0.5F}, 1.0F, glm::vec3(1.0F), 1.0F);
    const auto lists = r::assign_lights_to_clusters(p, {&reaching, 1});
    EXPECT_GT(assigned_clusters(lists), 0U);
    const std::uint32_t max_slice = r::depth_slice(p, 0.5F + 1e-3F);
    for (std::uint32_t i = 0; i < r::cluster_count; ++i) {
        if (!lists[i].empty()) EXPECT_LE(i / (r::cluster_grid_x * r::cluster_grid_y), max_slice);
    }

    // Zero range touches nothing anywhere.
    const r::GpuLight dead = r::make_point_light({0.0F, 0.0F, -10.0F}, 0.0F, glm::vec3(1.0F), 1.0F);
    EXPECT_EQ(assigned_clusters(r::assign_lights_to_clusters(p, {&dead, 1})), 0U);
}

// The property shading relies on: for any visible point, every light whose
// sphere contains the point is in the list of the cluster the point maps to.
// Checked with a moved camera, so the light positions go through `view`.
TEST(LightClusters, EveryLightReachingAPointIsInItsCluster) {
    const glm::mat4 view = glm::lookAt(glm::vec3(4.0F, 3.0F, 9.0F), glm::vec3(0.0F, 1.0F, 0.0F),
                                       glm::vec3(0.0F, 1.0F, 0.0F));
    const r::ClusterParams p = test_params(view);
    const glm::mat4 inv_view = glm::inverse(view);

    std::mt19937 rng(42);
    std::uniform_real_distribution<float> pos(-25.0F, 25.0F);
    std::uniform_real_distribution<float> rad(0.5F, 6.0F);
    std::vector<r::GpuLight> lights;
    for (int i = 0; i < 120; ++i) {
        lights.push_back(r::make_point_light({pos(rng), pos(rng) * 0.3F, pos(rng)}, rad(rng),
                                             glm::vec3(1.0F), 1.0F));
    }
    const auto lists = r::assign_lights_to_clusters(p, lights);

    std::uniform_real_distribution<float> unit(0.0F, 1.0F);
    std::uniform_int_distribution<std::uint32_t> cx(0, r::cluster_grid_x - 1);
    std::uniform_int_distribution<std::uint32_t> cy(0, r::cluster_grid_y - 1);
    std::uniform_int_distribution<std::uint32_t> cz(0, 16); // within ~40 m of the camera
    int checked = 0;
    for (int s = 0; s < 4000; ++s) {
        const glm::uvec3 c(cx(rng), cy(rng), cz(rng));
        const glm::vec3 v = point_in_cluster(p, c, {unit(rng), unit(rng), unit(rng)});
        const glm::vec3 world = glm::vec3(inv_view * glm::vec4(v, 1.0F));
        const auto& list = lists[r::cluster_index(r::cluster_of_view_position(p, v))];
        for (std::uint32_t i = 0; i < lights.size(); ++i) {
            const float d = glm::distance(world, glm::vec3(lights[i].position_range));
            if (d < lights[i].position_range.w * 0.999F) {
                ++checked;
                EXPECT_TRUE(std::binary_search(list.begin(), list.end(), i))
                    << "light " << i << " reaches the point but is not in its cluster";
            }
        }
    }
    EXPECT_GT(checked, 100); // the sample actually exercised lit points
    // ...and culling still culls: far fewer than all pairs are assigned.
    std::size_t total = 0;
    for (const auto& l : lists) total += l.size();
    EXPECT_LT(total, static_cast<std::size_t>(r::cluster_count) * lights.size() / 10);
}

} // namespace
