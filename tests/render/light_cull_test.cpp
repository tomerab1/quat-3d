// GPU clustered light culling (R10.1): the light_cull compute pass's cluster
// lists, read back and compared with the CPU reference.

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <iterator>
#include <random>
#include <utility>
#include <vector>

#include <glm/glm.hpp>
#include <glm/gtc/matrix_transform.hpp>
#include <gtest/gtest.h>
#include <vulkan/vulkan.h>

#include "engine/renderer/light_clusters.hpp"
#include "engine/renderer/light_cull_pass.hpp"
#include "engine/rhi/device.hpp"
#include "engine/rhi/gpu_allocator.hpp"
#include "engine/rhi/pipeline_cache.hpp"
#include "engine/rhi/render_graph.hpp"
#include "support/expect_ok.hpp"
#include "support/gpu_context.hpp"
#include "support/gpu_submit.hpp"

namespace {

namespace r = engine::renderer;
namespace rhi = engine::rhi;

glm::mat4 vulkan_perspective(float fov_y, float aspect, float near_z, float far_z) {
    glm::mat4 p = glm::perspective(fov_y, aspect, near_z, far_z);
    p[1][1] *= -1.0F;
    return p;
}

// What the cull pass wrote, read back to the host.
struct CullReadback {
    std::vector<std::uint32_t> grid;    // (offset, count) pairs
    std::vector<std::uint32_t> indices;
    std::uint32_t              requested = 0;

    [[nodiscard]] std::vector<std::uint32_t> list(std::uint32_t cluster) const {
        const std::uint32_t offset = grid[cluster * 2];
        const std::uint32_t count = grid[cluster * 2 + 1];
        return {indices.begin() + offset, indices.begin() + offset + count};
    }
};

[[nodiscard]] std::expected<rhi::GpuBuffer, engine::core::Error>
readback_buffer(rhi::GpuAllocator& allocator, VkDeviceSize bytes) {
    return allocator.create_buffer(
        bytes, VK_BUFFER_USAGE_TRANSFER_DST_BIT, VMA_MEMORY_USAGE_AUTO,
        VMA_ALLOCATION_CREATE_HOST_ACCESS_RANDOM_BIT | VMA_ALLOCATION_CREATE_MAPPED_BIT);
}

// Runs the cull pass for `lights` seen by (view, proj) and reads back its
// output. The read-back copy is a graph pass reading the cluster buffers as
// transfer sources, so the graph's compute -> transfer buffer barriers are
// exercised too.
[[nodiscard]] std::expected<CullReadback, engine::core::Error>
run_cull(quat::test::GpuContext& g, r::LightCullPass& pass, const glm::mat4& view,
         const glm::mat4& proj, const std::vector<r::GpuLight>& lights) {
    constexpr VkDeviceSize grid_bytes = r::cluster_count * 2 * sizeof(std::uint32_t);
    constexpr VkDeviceSize index_bytes = r::max_cluster_light_indices * sizeof(std::uint32_t);
    auto grid = readback_buffer(g.allocator, grid_bytes);
    if (!grid) return std::unexpected(grid.error());
    auto indices = readback_buffer(g.allocator, index_bytes);
    if (!indices) return std::unexpected(indices.error());
    auto counter = readback_buffer(g.allocator, sizeof(std::uint32_t));
    if (!counter) return std::unexpected(counter.error());

    rhi::RenderGraph graph; // buffers only: no transient image pool needed
    auto clustered = pass.add_to_graph(graph, view, proj, lights);
    if (!clustered) return std::unexpected(clustered.error());
    const r::ClusteredLights cl = *clustered;
    const VkBuffer grid_dst = grid->handle();
    const VkBuffer index_dst = indices->handle();
    const VkBuffer counter_dst = counter->handle();
    (void)graph.add_pass("readback", rhi::PassType::transfer)
        .reads(cl.grid, rhi::ResourceUsage::transfer_src)
        .reads(cl.indices, rhi::ResourceUsage::transfer_src)
        .reads(cl.counter, rhi::ResourceUsage::transfer_src)
        .execute([=](rhi::PassContext& ctx) {
            const VkBufferCopy g_copy{0, 0, grid_bytes};
            vkCmdCopyBuffer(ctx.cmd(), ctx.resolve(cl.grid).buffer, grid_dst, 1, &g_copy);
            const VkBufferCopy i_copy{0, 0, index_bytes};
            vkCmdCopyBuffer(ctx.cmd(), ctx.resolve(cl.indices).buffer, index_dst, 1, &i_copy);
            const VkBufferCopy c_copy{0, 0, sizeof(std::uint32_t)};
            vkCmdCopyBuffer(ctx.cmd(), ctx.resolve(cl.counter).buffer, counter_dst, 1, &c_copy);
        });
    if (auto c = graph.compile(); !c) return std::unexpected(c.error());
    auto done = quat::test::submit_and_wait(g.device, [&](VkCommandBuffer cmd) {
        graph.execute(cmd);
        quat::test::transfer_to_host_barrier(cmd);
    });
    if (!done) return std::unexpected(done.error());

    CullReadback out;
    out.grid.resize(r::cluster_count * 2);
    std::memcpy(out.grid.data(), grid->mapped(), grid_bytes);
    out.indices.resize(r::max_cluster_light_indices);
    std::memcpy(out.indices.data(), indices->mapped(), index_bytes);
    std::memcpy(&out.requested, counter->mapped(), sizeof(std::uint32_t));
    return out;
}

TEST(GpuLightCull, ClusterListsMatchTheCpuReference) {
    QUAT_REQUIRE_GPU(g);
    auto pass = r::LightCullPass::create(g->device, g->allocator, g->cache, g->shader_dir);
    ASSERT_OK(pass);

    const glm::mat4 view = glm::lookAt(glm::vec3(3.0F, 4.0F, 12.0F), glm::vec3(0.0F, 1.0F, 0.0F),
                                       glm::vec3(0.0F, 1.0F, 0.0F));
    const glm::mat4 proj = vulkan_perspective(glm::radians(60.0F), 16.0F / 9.0F, 0.1F, 150.0F);

    std::mt19937 rng(1234);
    std::uniform_real_distribution<float> pos(-30.0F, 30.0F);
    std::uniform_real_distribution<float> rad(0.3F, 8.0F);
    std::vector<r::GpuLight> lights;
    for (int i = 0; i < 300; ++i) {
        lights.push_back(r::make_point_light({pos(rng), pos(rng) * 0.25F, pos(rng)}, rad(rng),
                                             glm::vec3(1.0F), 1.0F));
    }
    // Behind the camera (out of view), at zero range, and huge.
    const glm::vec3 behind = glm::vec3(glm::inverse(view) * glm::vec4(0.0F, 0.0F, 6.0F, 1.0F));
    lights.push_back(r::make_point_light(behind, 2.0F, glm::vec3(1.0F), 1.0F));
    lights.push_back(r::make_point_light({0.0F, 1.0F, 0.0F}, 0.0F, glm::vec3(1.0F), 1.0F));
    lights.push_back(r::make_point_light({0.0F, 1.0F, 0.0F}, 60.0F, glm::vec3(1.0F), 1.0F));

    auto got = run_cull(*g, *pass, view, proj, lights);
    ASSERT_OK(got);

    auto params = r::make_cluster_params(view, proj, static_cast<std::uint32_t>(lights.size()));
    ASSERT_OK(params);
    const auto expected = r::assign_lights_to_clusters(*params, lights);

    std::size_t total = 0;
    int borderline = 0;
    for (std::uint32_t c = 0; c < r::cluster_count; ++c) {
        const std::vector<std::uint32_t> gpu = got->list(c);
        const std::vector<std::uint32_t>& cpu = expected[c];
        total += gpu.size();
        ASSERT_TRUE(std::is_sorted(gpu.begin(), gpu.end())) << "cluster " << c;
        if (gpu == cpu) continue;
        // Any difference must be a light grazing the cluster's box, where
        // GPU and CPU float rounding may legitimately disagree.
        std::vector<std::uint32_t> diff;
        std::set_symmetric_difference(gpu.begin(), gpu.end(), cpu.begin(), cpu.end(),
                                      std::back_inserter(diff));
        const glm::uvec3 coord(c % r::cluster_grid_x, (c / r::cluster_grid_x) % r::cluster_grid_y,
                               c / (r::cluster_grid_x * r::cluster_grid_y));
        const r::ClusterAabb box = r::cluster_aabb(*params, coord);
        for (const std::uint32_t i : diff) {
            const glm::vec3 centre(view * glm::vec4(glm::vec3(lights[i].position_range), 1.0F));
            const float r2 = lights[i].position_range.w * lights[i].position_range.w;
            const float margin = std::abs(r::aabb_distance_sq(box, centre) - r2) / r2;
            EXPECT_LT(margin, 1e-3F) << "cluster " << c << " light " << i;
            ++borderline;
        }
    }
    EXPECT_LT(borderline, 10);
    EXPECT_EQ(got->requested, total); // every cluster reserved exactly its count
    EXPECT_GT(total, 0U);
    // Lists are packed without overlap: their ranges tile [0, total).
    std::vector<std::pair<std::uint32_t, std::uint32_t>> ranges;
    for (std::uint32_t c = 0; c < r::cluster_count; ++c) {
        if (got->grid[c * 2 + 1] > 0) ranges.emplace_back(got->grid[c * 2], got->grid[c * 2 + 1]);
    }
    std::sort(ranges.begin(), ranges.end());
    std::uint32_t cursor = 0;
    for (const auto& [offset, count] : ranges) {
        EXPECT_EQ(offset, cursor);
        cursor = offset + count;
    }
    EXPECT_EQ(cursor, total);
}

// Re-running the pass on new lights (same instance, next frame) resets the
// cursor and replaces every list.
TEST(GpuLightCull, SecondRunReplacesTheLists) {
    QUAT_REQUIRE_GPU(g);
    auto pass = r::LightCullPass::create(g->device, g->allocator, g->cache, g->shader_dir);
    ASSERT_OK(pass);
    const glm::mat4 view(1.0F);
    const glm::mat4 proj = vulkan_perspective(glm::radians(60.0F), 1.0F, 0.1F, 100.0F);

    const std::vector<r::GpuLight> first{
        r::make_point_light({0.0F, 0.0F, -10.0F}, 3.0F, glm::vec3(1.0F), 1.0F)};
    ASSERT_OK(run_cull(*g, *pass, view, proj, first));

    const std::vector<r::GpuLight> none{};
    auto empty = run_cull(*g, *pass, view, proj, none);
    ASSERT_OK(empty);
    EXPECT_EQ(empty->requested, 0U);
    for (std::uint32_t c = 0; c < r::cluster_count; ++c) {
        EXPECT_EQ(empty->grid[c * 2 + 1], 0U) << "cluster " << c;
    }
}

// Bindings come from reflection (by name); a hot reload of the unchanged
// shader passes the R2 compatibility check and the rebuilt pipeline still
// culls with the descriptors written at creation.
TEST(GpuLightCull, HotReloadKeepsTheReflectedBindings) {
    QUAT_REQUIRE_GPU(g);
    auto pass = r::LightCullPass::create(g->device, g->allocator, g->cache, g->shader_dir);
    ASSERT_OK(pass);
    auto report = g->cache.reload(g->shader_dir + "/light_cull.spv");
    ASSERT_OK(report);
    EXPECT_TRUE(report->errors.empty()) << report->errors.front();
    EXPECT_GE(report->rebuilt, 1U);

    const glm::mat4 view(1.0F);
    const glm::mat4 proj = vulkan_perspective(glm::radians(60.0F), 1.0F, 0.1F, 100.0F);
    const std::vector<r::GpuLight> lights{
        r::make_point_light({0.0F, 0.0F, -10.0F}, 3.0F, glm::vec3(1.0F), 1.0F)};
    auto got = run_cull(*g, *pass, view, proj, lights);
    ASSERT_OK(got);
    EXPECT_GT(got->requested, 0U) << "the reloaded pipeline culled nothing";
}

// max_lights lights that each cover the whole view: the requested total
// exceeds the index list, which stays within capacity (truncated lists), and
// the counter reports the overflow.
TEST(GpuLightCull, OverflowIsClampedToTheIndexCapacity) {
    QUAT_REQUIRE_GPU(g);
    auto pass = r::LightCullPass::create(g->device, g->allocator, g->cache, g->shader_dir);
    ASSERT_OK(pass);
    const glm::mat4 view(1.0F);
    const glm::mat4 proj = vulkan_perspective(glm::radians(60.0F), 1.0F, 0.1F, 50.0F);
    std::vector<r::GpuLight> lights(r::max_lights + 10,
                                    r::make_point_light({0.0F, 0.0F, -20.0F}, 500.0F,
                                                        glm::vec3(1.0F), 1.0F));
    auto got = run_cull(*g, *pass, view, proj, lights);
    ASSERT_OK(got);
    // Only max_lights are uploaded; each reaches every cluster.
    EXPECT_EQ(got->requested, r::cluster_count * r::max_lights);
    std::uint64_t stored = 0;
    for (std::uint32_t c = 0; c < r::cluster_count; ++c) {
        const std::uint32_t offset = got->grid[c * 2];
        const std::uint32_t count = got->grid[c * 2 + 1];
        EXPECT_LE(count, r::max_lights);
        if (count > 0) EXPECT_LE(offset + count, r::max_cluster_light_indices);
        stored += count;
    }
    EXPECT_EQ(stored, r::max_cluster_light_indices);
}

} // namespace
