// GPU terrain tiles end to end (R10.2): the streamer generates through the
// terrain pass's GPU backend, the read-back heightmap (what physics and the
// navmesh consume) matches the CPU reference, set_tile adopts the image the
// GPU wrote, and the tile renders; unsupported params and blocking updates
// fall back to the CPU.

#include <chrono>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <future>
#include <optional>
#include <vector>

#include <glm/glm.hpp>
#include <glm/gtc/matrix_transform.hpp>
#include <gtest/gtest.h>
#include <vulkan/vulkan.h>

#include "engine/renderer/mesh_pass.hpp"
#include "engine/renderer/terrain_gpu_tiles.hpp"
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

namespace engine::renderer {

// Issue #78: fails every pending tile exactly as a failed synthesis job does
// (the GPU job stops, the tile is regenerated on a CPU worker), through the
// source's own fallback, without a fault-injection switch in the engine.
struct GpuTileSourceTestAccess {
    static void fail_pending(GpuTileSource& src) {
        std::vector<std::uint64_t> ids;
        for (const auto& [id, pending] : src.pending_) ids.push_back(id);
        for (const std::uint64_t id : ids) {
            src.synth_.cancel(id);
            src.fall_back_to_cpu(id);
        }
    }
};

} // namespace engine::renderer

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

        terrain.tick(); // per-frame upkeep: deliveries, cancellations, aging
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

// Runs GpuTileSource frames without any drawing: tick + synthesis graph.
void source_frame(quat::test::GpuContext& g, r::GpuTileSource& source) {
    source.tick();
    rhi::RenderGraph graph;
    source.add_to_graph(graph);
    ASSERT_OK(graph.compile());
    ASSERT_OK(quat::test::submit_and_wait(g.device, [&](VkCommandBuffer cmd) {
        graph.execute(cmd);
    }));
}

// PR #3 review (1): a dropped request must not keep the one-at-a-time
// synthesis queue busy. Cancelling its token stops the job (queued or part
// way through) at the next tick and resolves its future invalid.
TEST(GpuTerrainTiles, CancelledRequestFreesTheSynthesisQueue) {
    QUAT_REQUIRE_GPU(g);
    auto source = r::GpuTileSource::create(g->device, g->allocator, g->cache, g->shader_dir);
    ASSERT_OK(source);
    r::GpuTileSource& src = **source;
    using namespace std::chrono_literals;

    t::TerrainParams huge = tile_params(); // ~300 frames at the default budget
    huge.resolution = 1025;
    huge.tile_size_m = 2000.0F;
    huge.grid.iterations = t::max_grid_erosion_steps;
    t::CancelToken huge_token = t::make_cancel_token();
    auto huge_job = src.generate(huge, huge_token);
    t::CancelToken queued_token = t::make_cancel_token();
    auto queued_job = src.generate(tile_params(), queued_token);
    source_frame(*g, src); // the huge job starts
    ASSERT_EQ(huge_job.wait_for(0s), std::future_status::timeout);

    huge_token->store(true);
    queued_token->store(true);
    src.tick();
    ASSERT_EQ(huge_job.wait_for(0s), std::future_status::ready);
    EXPECT_FALSE(huge_job.get().valid());
    ASSERT_EQ(queued_job.wait_for(0s), std::future_status::ready);
    EXPECT_FALSE(queued_job.get().valid());
    EXPECT_FALSE(src.busy()) << "cancelled work still occupies the queue";

    auto wanted = src.generate(tile_params(), t::make_cancel_token());
    int frames = 0;
    for (; frames < 10 && wanted.wait_for(0s) != std::future_status::ready; ++frames) {
        source_frame(*g, src);
    }
    src.tick();
    ASSERT_EQ(wanted.wait_for(0s), std::future_status::ready) << "waited behind cancelled work";
    EXPECT_TRUE(wanted.get().valid());
}

// The same through the streamer: replacing it (every streaming regenerate)
// cancels its GPU job, so the new streamer's tile is not stuck behind it.
TEST(GpuTerrainTiles, ReplacedStreamerCancelsItsGpuJob) {
    QUAT_REQUIRE_GPU(g);
    auto source = r::GpuTileSource::create(g->device, g->allocator, g->cache, g->shader_dir);
    ASSERT_OK(source);
    r::GpuTileSource& src = **source;

    t::TerrainParams huge = tile_params();
    huge.resolution = 1025;
    huge.tile_size_m = 2000.0F;
    huge.grid.iterations = t::max_grid_erosion_steps;
    std::optional<t::TerrainStreamer> streamer;
    streamer.emplace(huge, 0);
    streamer->set_generator(&src);
    (void)streamer->update(glm::vec2(0.0F));
    source_frame(*g, src); // the huge tile is being synthesised

    t::TerrainParams fresh = tile_params();
    fresh.seed = 99;
    streamer.emplace(fresh, 0); // regenerate: the old streamer is destroyed
    streamer->set_generator(&src);
    bool ready = false;
    for (int frame = 0; frame < 10 && !ready; ++frame) {
        ready = !streamer->update(glm::vec2(0.0F)).ready.empty();
        if (!ready) source_frame(*g, src);
    }
    EXPECT_TRUE(ready) << "the new tile queued behind the replaced streamer's job";
}

// Issue #78: a tile whose GPU job fails is regenerated on a CPU worker that
// keeps the request's cancel token: cancelling the tile, abandon() and
// destroying the source all stop that generation instead of waiting out
// hours of erosion.
TEST(GpuTerrainTiles, CpuFallbackHonoursCancellation) {
    QUAT_REQUIRE_GPU(g);
    using namespace std::chrono_literals;
    t::TerrainParams endless = tile_params(); // hours on a CPU worker
    endless.resolution = 1025;
    endless.tile_size_m = 2000.0F;
    endless.grid.iterations = t::max_grid_erosion_steps;
    const auto t0 = std::chrono::steady_clock::now();
    std::future<t::Heightmap> abandoned;
    {
        auto source = r::GpuTileSource::create(g->device, g->allocator, g->cache, g->shader_dir);
        ASSERT_OK(source);
        r::GpuTileSource& src = **source;
        const auto fail_gpu = [&] { r::GpuTileSourceTestAccess::fail_pending(src); };

        // The requester cancels after the job fell back to the CPU.
        t::CancelToken token = t::make_cancel_token();
        auto cancelled = src.generate(endless, token);
        source_frame(*g, src); // the job starts on the GPU ...
        fail_gpu();            // ... fails, and is handed to a CPU worker
        ASSERT_EQ(src.cpu_fallback_count(), 1U);
        EXPECT_FALSE(src.busy()) << "the fallback still counts as GPU work";
        EXPECT_EQ(cancelled.wait_for(50ms), std::future_status::timeout);
        token->store(true);
        ASSERT_EQ(cancelled.wait_for(30s), std::future_status::ready)
            << "the CPU fallback ignored the request's cancel token";
        EXPECT_FALSE(cancelled.get().valid());

        // abandon() gives up a fallback the requester still wants.
        auto given_up = src.generate(endless, t::make_cancel_token());
        fail_gpu();
        src.abandon();
        ASSERT_EQ(given_up.wait_for(30s), std::future_status::ready);
        EXPECT_FALSE(given_up.get().valid());

        // A fallback still running when the source is destroyed (no token).
        abandoned = src.generate(endless, nullptr);
        fail_gpu();
        ASSERT_GE(src.cpu_fallback_count(), 1U);
        EXPECT_EQ(abandoned.wait_for(0s), std::future_status::timeout);
    } // ~GpuTileSource
    ASSERT_EQ(abandoned.wait_for(0s), std::future_status::ready);
    EXPECT_FALSE(abandoned.get().valid());
    EXPECT_LT(std::chrono::steady_clock::now() - t0, 60s) << "shutdown waited for the fallbacks";
}

// PR #3 review (2): freeing finished jobs and expiring unclaimed tile images
// runs in tick(), so it happens even when terrain no longer draws (the
// Terrain entity was deleted after the job finished).
TEST(GpuTerrainTiles, FinishedWorkIsFreedWithoutDrawing) {
    QUAT_REQUIRE_GPU(g);
    auto source = r::GpuTileSource::create(g->device, g->allocator, g->cache, g->shader_dir);
    ASSERT_OK(source);
    r::GpuTileSource& src = **source;
    using namespace std::chrono_literals;

    auto job = src.generate(tile_params(), t::make_cancel_token());
    for (int i = 0; i < 10 && job.wait_for(0s) != std::future_status::ready; ++i) {
        source_frame(*g, src);
    }
    src.tick();
    ASSERT_EQ(job.wait_for(0s), std::future_status::ready);
    ASSERT_TRUE(job.get().valid()); // delivered, never adopted
    EXPECT_EQ(src.parked_count(), 1U);
    EXPECT_GE(src.retired_count(), 1U);

    // No add_to_graph from here on: only the per-frame tick.
    for (int i = 0; i < r::GpuTileSource::unclaimed_frames + 4; ++i) src.tick();
    EXPECT_EQ(src.parked_count(), 0U) << "unclaimed tile image never expired";
    EXPECT_EQ(src.retired_count(), 0U) << "finished job's scratch never freed";
}

// Audit D1: a GPU tile's future only progresses while frames run, so
// teardown (or deleting the Terrain entity) abandons it. Its future must then
// resolve at once — invalid, "cancelled" — whether the job was still queued
// or already part-way through on the GPU, and later frames and generations
// must carry on normally.
TEST(GpuTerrainTiles, PendingGpuTileIsAbandonedCleanly) {
    QUAT_REQUIRE_GPU(g);
    auto mesh = r::MeshPass::create(g->device, g->allocator, g->cache, g->transfer, g->shader_dir);
    ASSERT_OK(mesh);
    auto terrain =
        r::TerrainPass::create(g->device, g->allocator, g->cache, g->transfer, g->shader_dir);
    ASSERT_OK(terrain);
    t::TileGenerator* gpu = terrain->tile_generator();
    ASSERT_NE(gpu, nullptr);
    auto px = g->allocator.create_buffer(
        4, VK_BUFFER_USAGE_TRANSFER_DST_BIT, VMA_MEMORY_USAGE_AUTO,
        VMA_ALLOCATION_CREATE_HOST_ACCESS_RANDOM_BIT | VMA_ALLOCATION_CREATE_MAPPED_BIT);
    ASSERT_OK(px);
    Frame frame{*mesh, *terrain};
    using namespace std::chrono_literals;

    // Queued, never started: no frame has run.
    auto queued = t::generate_tile_async(gpu, tile_params());
    EXPECT_TRUE(terrain->has_tiles());
    EXPECT_EQ(queued.wait_for(0s), std::future_status::timeout);
    terrain->abandon_pending_tiles();
    ASSERT_EQ(queued.wait_for(0s), std::future_status::ready);
    EXPECT_FALSE(queued.get().valid());
    EXPECT_FALSE(terrain->has_tiles()) << "an abandoned job still keeps frames busy";

    // Started: one frame records the first erosion slice (default budget
    // spreads a big tile over frames), then the tile is abandoned mid-way.
    t::TerrainParams big = tile_params();
    big.resolution = 513;
    big.tile_size_m = 1000.0F;
    big.grid.iterations = 200; // > one frame's budget (64 steps at 513²)
    auto started = t::generate_tile_async(gpu, big);
    ASSERT_TRUE(frame.run(*g, *px).has_value());
    EXPECT_EQ(started.wait_for(0s), std::future_status::timeout);
    terrain->abandon_pending_tiles();
    ASSERT_EQ(started.wait_for(0s), std::future_status::ready);
    EXPECT_FALSE(started.get().valid());
    // The in-flight GPU job drains (frames keep driving it) and is discarded.
    for (int i = 0; i < 40 && terrain->has_tiles(); ++i) {
        ASSERT_TRUE(frame.run(*g, *px).has_value());
    }
    EXPECT_FALSE(terrain->has_tiles()) << "abandoned GPU work never drained";

    // A fresh request after abandoning is delivered as usual.
    auto again = t::generate_tile_async(gpu, tile_params());
    for (int i = 0; i < 20 && again.wait_for(0s) != std::future_status::ready; ++i) {
        ASSERT_TRUE(frame.run(*g, *px).has_value());
    }
    ASSERT_EQ(again.wait_for(0s), std::future_status::ready);
    const t::Heightmap map = again.get();
    EXPECT_TRUE(map.valid());
    EXPECT_NE(map.gpu_tile, 0U);
}

} // namespace
