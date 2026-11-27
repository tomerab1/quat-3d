// GPU terrain tiles end to end (R10.2): the streamer generates through the
// terrain pass's GPU backend, the read-back heightmap (what physics and the
// navmesh consume) matches the CPU reference, set_tile adopts the image the
// GPU wrote, and the tile renders; unsupported params and blocking updates
// fall back to the CPU.

#include <cmath>
#include <cstdint>
#include <cstdio>
#include <optional>
#include <vector>

#include <glm/glm.hpp>
#include <glm/gtc/matrix_transform.hpp>
#include <gtest/gtest.h>
#include <vulkan/vulkan.h>

#include "engine/renderer/mesh_pass.hpp"
#include "engine/renderer/terrain_pass.hpp"
#include "engine/rhi/device.hpp"
#include "engine/rhi/gpu_allocator.hpp"
#include "engine/rhi/render_graph.hpp"
#include "engine/terrain/generator.hpp"
#include "engine/terrain/streamer.hpp"
#include "engine/terrain/tile_generator.hpp"
#include "support/expect_ok.hpp"
#include "support/gpu_context.hpp"
#include "support/gpu_submit.hpp"

namespace {

namespace r = engine::renderer;
namespace rhi = engine::rhi;
namespace t = engine::terrain;

[[nodiscard]] t::TerrainParams tile_params() {
    t::TerrainParams p;
    p.resolution = 129;
    p.tile_size_m = 256.0F;
    p.height_m = 40.0F;
    p.octaves = 5;
    p.erosion_model = t::ErosionModel::grid;
    p.grid.iterations = 40;
    return p;
}

// One frame: clear the GBuffer (mesh pass, no draws), the terrain pass
// (synthesis + tile draws), then read back the centre albedo texel.
struct Frame {
    r::MeshPass&    mesh;
    r::TerrainPass& terrain;

    [[nodiscard]] std::optional<std::uint8_t> run(quat::test::GpuContext& g, rhi::GpuBuffer& px) {
        constexpr VkExtent2D extent{64, 64};
        const glm::vec3 eye(0.0F, 300.0F, 0.0F);
        const glm::mat4 view =
            glm::lookAt(eye, glm::vec3(0.0F), glm::vec3(0.0F, 0.0F, 1.0F));
        glm::mat4 proj = glm::perspective(glm::radians(60.0F), 1.0F, 0.1F, 1000.0F);
        proj[1][1] *= -1.0F;
        const glm::mat4 vp = proj * view;

        rhi::TransientImagePool pool(g.device, g.allocator);
        rhi::RenderGraph graph(pool);
        auto targets = mesh.add_to_graph(graph, extent, vp, r::InstancedDrawList{});
        if (!targets) return std::nullopt;
        if (terrain.has_tiles()) {
            terrain.add_to_graph(graph, *targets, extent, vp, eye, glm::vec3(0.0F), 1000.0F);
        }
        if (!graph.compile()) return std::nullopt;
        const VkImage albedo = graph.binding(targets->albedo).image;
        auto done = quat::test::submit_and_wait(g.device, [&](VkCommandBuffer cmd) {
            graph.execute(cmd);
            VkImageMemoryBarrier2 b{};
            b.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER_2;
            b.srcStageMask = VK_PIPELINE_STAGE_2_COLOR_ATTACHMENT_OUTPUT_BIT;
            b.srcAccessMask = VK_ACCESS_2_COLOR_ATTACHMENT_WRITE_BIT;
            b.dstStageMask = VK_PIPELINE_STAGE_2_COPY_BIT;
            b.dstAccessMask = VK_ACCESS_2_TRANSFER_READ_BIT;
            b.oldLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
            b.newLayout = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL;
            b.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
            b.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
            b.image = albedo;
            b.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
            VkDependencyInfo dep{};
            dep.sType = VK_STRUCTURE_TYPE_DEPENDENCY_INFO;
            dep.imageMemoryBarrierCount = 1;
            dep.pImageMemoryBarriers = &b;
            vkCmdPipelineBarrier2(cmd, &dep);
            VkBufferImageCopy region{};
            region.imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
            region.imageOffset = {static_cast<std::int32_t>(extent.width / 2),
                                  static_cast<std::int32_t>(extent.height / 2), 0};
            region.imageExtent = {1, 1, 1};
            vkCmdCopyImageToBuffer(cmd, albedo, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
                                   px.handle(), 1, &region);
            quat::test::transfer_to_host_barrier(cmd);
        });
        if (!done) return std::nullopt;
        if (!px.invalidate_mapped()) return std::nullopt;
        return static_cast<const std::uint8_t*>(px.mapped())[3]; // albedo alpha
    }
};

TEST(GpuTerrainTiles, StreamedTileIsSynthesisedAdoptedAndRendered) {
    QUAT_REQUIRE_GPU(g);
    auto mesh = r::MeshPass::create(g->device, g->allocator, g->cache, g->transfer, g->shader_dir);
    ASSERT_OK(mesh);
    auto terrain =
        r::TerrainPass::create(g->device, g->allocator, g->cache, g->transfer, g->shader_dir);
    ASSERT_OK(terrain);
    ASSERT_NE(terrain->tile_generator(), nullptr) << "GPU synthesis unavailable";
    auto px = g->allocator.create_buffer(
        4, VK_BUFFER_USAGE_TRANSFER_DST_BIT, VMA_MEMORY_USAGE_AUTO,
        VMA_ALLOCATION_CREATE_HOST_ACCESS_RANDOM_BIT | VMA_ALLOCATION_CREATE_MAPPED_BIT);
    ASSERT_OK(px);

    const t::TerrainParams params = tile_params();
    t::TerrainStreamer streamer(params, 0);
    streamer.set_generator(terrain->tile_generator());
    Frame frame{*mesh, *terrain};

    // Frames run until the tile is in and drawn: update -> set_tile -> frame.
    std::optional<std::uint8_t> alpha;
    bool adopted = false;
    int frames = 0;
    for (; frames < 50 && !adopted; ++frames) {
        for (const glm::ivec2& c : streamer.update(glm::vec2(0.0F)).ready) {
            const t::Heightmap& map = streamer.maps().at(c);
            EXPECT_NE(map.gpu_tile, 0U) << "tile generated on the CPU, not the GPU";
            ASSERT_OK(terrain->set_tile(c, map, g->transfer));
            adopted = true;
        }
        alpha = frame.run(*g, *px);
        ASSERT_TRUE(alpha.has_value());
    }
    ASSERT_TRUE(adopted) << "no tile after " << frames << " frames";
    std::printf("[terrain tiles] GPU tile adopted after %d frames\n", frames);
    EXPECT_GE(*alpha, 200) << "the adopted tile did not rasterise";

    // The read-back heights (physics / navmesh input) are the CPU reference.
    const t::Heightmap& gpu = streamer.maps().at(glm::ivec2(0, 0));
    const t::Heightmap cpu = t::generate(params);
    ASSERT_EQ(gpu.heights.size(), cpu.heights.size());
    float worst = 0.0F;
    for (std::size_t i = 0; i < cpu.heights.size(); ++i) {
        worst = std::max(worst, std::abs(gpu.heights[i] - cpu.heights[i]));
    }
    EXPECT_LT(worst, 0.01F) << "read-back heights drifted from the CPU reference";
}

TEST(GpuTerrainTiles, DropletTilesAndBlockingUpdatesFallBackToTheCpu) {
    QUAT_REQUIRE_GPU(g);
    auto terrain =
        r::TerrainPass::create(g->device, g->allocator, g->cache, g->transfer, g->shader_dir);
    ASSERT_OK(terrain);
    t::TileGenerator* gpu = terrain->tile_generator();
    ASSERT_NE(gpu, nullptr);

    t::TerrainParams droplets = tile_params();
    droplets.erosion_model = t::ErosionModel::droplets;
    droplets.erosion_droplets = 500;
    EXPECT_FALSE(gpu->supports(droplets));
    const t::Heightmap map = t::generate_tile_async(gpu, droplets).get(); // CPU worker
    ASSERT_TRUE(map.valid());
    EXPECT_EQ(map.gpu_tile, 0U);
    EXPECT_OK(terrain->set_tile(glm::ivec2(0, 0), map, g->transfer)); // uploads

    // A blocking update cannot wait for frames: the window fills on the CPU.
    t::TerrainStreamer streamer(tile_params(), 0);
    streamer.set_generator(gpu);
    (void)streamer.update(glm::vec2(0.0F));  // queues a GPU job
    EXPECT_TRUE(terrain->has_tiles());
    const auto ev = streamer.update(glm::vec2(0.0F), /*block=*/true);
    ASSERT_EQ(ev.ready.size(), 1U);
    EXPECT_EQ(streamer.maps().at(glm::ivec2(0, 0)).gpu_tile, 0U);
}

} // namespace
