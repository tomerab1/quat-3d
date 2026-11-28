// GPU terrain synthesis (R10.2): the terrain_synth compute pass against the
// CPU reference generator — noise agreement, grid-erosion agreement, seams
// between GPU tiles, run-to-run determinism, frame-sliced budgets, the tile
// image, and a timing smoke test.
//
// Accuracy contract (documented in terrain_synth_pass.hpp): the noise field
// matches terrain::generate to float rounding — GPU division / sqrt are not
// correctly rounded and drivers may fuse multiply-adds — so heights agree to
// within k_noise_tolerance_m (measured differences are orders of magnitude
// smaller). Grid erosion is an iterated simulation whose branches (erode vs
// deposit) can flip on those last bits, so it is compared statistically.
// Edges between GPU tiles are compared bit for bit.

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <map>
#include <vector>

#include <glm/glm.hpp>
#include <gtest/gtest.h>
#include <vulkan/vulkan.h>

#include "engine/renderer/terrain_synth_pass.hpp"
#include "engine/rhi/device.hpp"
#include "engine/rhi/gpu_allocator.hpp"
#include "engine/rhi/pipeline_cache.hpp"
#include "engine/rhi/render_graph.hpp"
#include "engine/terrain/generator.hpp"
#include "support/expect_ok.hpp"
#include "support/gpu_context.hpp"
#include "support/gpu_submit.hpp"

namespace {

namespace r = engine::renderer;
namespace rhi = engine::rhi;
namespace t = engine::terrain;

// Max |GPU - CPU| for the un-eroded field, as a fraction of height_m.
constexpr float k_noise_tolerance = 1e-4F;

[[nodiscard]] t::TerrainParams noise_params() {
    t::TerrainParams p;
    p.resolution = 129;
    p.tile_size_m = 256.0F;
    p.height_m = 60.0F;
    p.erosion_droplets = 0; // noise only
    return p;
}

[[nodiscard]] t::TerrainParams eroded_params() {
    t::TerrainParams p = noise_params();
    p.erosion_model = t::ErosionModel::grid;
    p.grid.iterations = 60;
    return p;
}

// Runs frames (graph build + blocking submit) until every queued job is
// collected; returns the results by job id.
[[nodiscard]] std::map<std::uint64_t, r::TerrainSynthPass::Result>
run_to_completion(quat::test::GpuContext& g, r::TerrainSynthPass& pass, int* frames = nullptr) {
    std::map<std::uint64_t, r::TerrainSynthPass::Result> out;
    int count = 0;
    while (!pass.idle() && count < 10'000) {
        rhi::RenderGraph graph;
        EXPECT_OK(pass.add_to_graph(graph));
        EXPECT_OK(graph.compile());
        EXPECT_OK(quat::test::submit_and_wait(g.device, [&](VkCommandBuffer cmd) {
            graph.execute(cmd);
        }));
        for (r::TerrainSynthPass::Result& res : pass.collect()) {
            const std::uint64_t id = res.id;
            out.emplace(id, std::move(res));
        }
        ++count;
    }
    if (frames != nullptr) *frames = count;
    return out;
}

[[nodiscard]] t::Heightmap synthesize(quat::test::GpuContext& g, r::TerrainSynthPass& pass,
                                      const t::TerrainParams& params) {
    const std::uint64_t id = pass.enqueue(params);
    EXPECT_NE(id, 0U);
    auto results = run_to_completion(g, pass);
    auto it = results.find(id);
    if (it == results.end()) return {};
    return std::move(it->second.heights);
}

[[nodiscard]] r::TerrainSynthPass make_pass(quat::test::GpuContext& g) {
    auto pass = r::TerrainSynthPass::create(g.device, g.allocator, g.cache, g.shader_dir);
    EXPECT_OK(pass);
    if (!pass) return {};
    pass->set_cell_budget(0); // whole jobs per frame unless a test slices
    return std::move(*pass);
}

struct Diff {
    float  max_abs = 0.0F;
    double mean_abs = 0.0;
};

[[nodiscard]] Diff diff(const t::Heightmap& a, const t::Heightmap& b) {
    Diff d;
    for (std::size_t i = 0; i < a.heights.size(); ++i) {
        const float e = std::abs(a.heights[i] - b.heights[i]);
        d.max_abs = std::max(d.max_abs, e);
        d.mean_abs += e;
    }
    d.mean_abs /= static_cast<double>(std::max<std::size_t>(a.heights.size(), 1));
    return d;
}

[[nodiscard]] bool bit_identical(const t::Heightmap& a, const t::Heightmap& b) {
    return a.heights.size() == b.heights.size() &&
           std::memcmp(a.heights.data(), b.heights.data(), a.heights.size() * sizeof(float)) == 0;
}

[[nodiscard]] bool same_bits(float a, float b) { return std::memcmp(&a, &b, sizeof(float)) == 0; }

TEST(GpuTerrainSynth, NoiseFieldMatchesTheCpuReference) {
    QUAT_REQUIRE_GPU(g);
    r::TerrainSynthPass pass = make_pass(*g);
    ASSERT_TRUE(pass.valid());

    for (const glm::ivec2 tile : {glm::ivec2(0, 0), glm::ivec2(-3, 7)}) {
        t::TerrainParams p = noise_params();
        p.world_tile = tile;
        const t::Heightmap gpu = synthesize(*g, pass, p);
        const t::Heightmap cpu = t::generate(p);
        ASSERT_TRUE(gpu.valid());
        ASSERT_EQ(gpu.heights.size(), cpu.heights.size());
        const Diff d = diff(gpu, cpu);
        std::printf("[terrain synth] tile (%d,%d) noise: max |gpu-cpu| %.3g m, mean %.3g m "
                    "(height %.0f m)\n",
                    tile.x, tile.y, static_cast<double>(d.max_abs), d.mean_abs,
                    static_cast<double>(p.height_m));
        EXPECT_LE(d.max_abs, k_noise_tolerance * p.height_m) << "tile " << tile.x << "," << tile.y;
        EXPECT_NEAR(gpu.min_height, cpu.min_height, k_noise_tolerance * p.height_m);
        EXPECT_NEAR(gpu.max_height, cpu.max_height, k_noise_tolerance * p.height_m);
    }
}

TEST(GpuTerrainSynth, GridErosionTracksTheCpuReference) {
    QUAT_REQUIRE_GPU(g);
    r::TerrainSynthPass pass = make_pass(*g);
    ASSERT_TRUE(pass.valid());

    const t::TerrainParams p = eroded_params();
    const t::Heightmap gpu = synthesize(*g, pass, p);
    const t::Heightmap cpu = t::generate(p);
    t::TerrainParams flat = p;
    flat.grid.iterations = 0;
    const t::Heightmap base = t::generate(flat);
    ASSERT_TRUE(gpu.valid());

    const Diff vs_cpu = diff(gpu, cpu);
    const Diff erosion = diff(cpu, base); // how much erosion changed the field
    std::printf("[terrain synth] grid erosion: mean |gpu-cpu| %.3g m (max %.3g m) vs mean "
                "erosion %.3g m (max %.3g m)\n",
                vs_cpu.mean_abs, static_cast<double>(vs_cpu.max_abs), erosion.mean_abs,
                static_cast<double>(erosion.max_abs));
    ASSERT_GT(erosion.mean_abs, 1e-4) << "erosion did nothing";
    // The two simulations agree far more closely than erosion moves terrain.
    EXPECT_LT(vs_cpu.mean_abs, 0.05 * erosion.mean_abs);
    EXPECT_LT(vs_cpu.max_abs, 0.25F * erosion.max_abs);

    // The border band is the pure noise field on both sides.
    const std::uint32_t res = p.resolution;
    for (std::uint32_t i = 0; i < res; ++i) {
        const std::size_t row_start = static_cast<std::size_t>(i) * res;
        for (const std::size_t k : {static_cast<std::size_t>(i), row_start}) {
            EXPECT_NEAR(gpu.heights[k], base.heights[k], k_noise_tolerance * p.height_m);
        }
    }
}

// Issue #69 / PR #90 review: the relax_* kernels and terrain::run_talus_op
// produce the same bits from the same input. With rain and thermal
// weathering off, the simulation steps leave the noise field bit for bit
// (every update adds an exact zero), so a GPU job without relaxation reads
// back the GPU's own noise field N. Feeding that N to the CPU reference
// (erode_grid: the same identity steps, then relax_talus) must reproduce,
// bit for bit, what the GPU job with relaxation reads back: N + (E - N)
// where the border fade is 1, N where it is 0.
TEST(GpuTerrainSynth, TalusRelaxationIsBitIdenticalToTheCpuReference) {
    QUAT_REQUIRE_GPU(g);
    r::TerrainSynthPass pass = make_pass(*g);
    ASSERT_TRUE(pass.valid());

    t::TerrainParams p = noise_params();
    p.erosion_model = t::ErosionModel::grid;
    p.grid.iterations = 1;
    p.grid.rain = 0.0F;
    p.grid.thermal_rate = 0.0F;
    p.grid.talus = 0.25F; // the 60 m relief has plenty steeper than 14°
    ASSERT_EQ(p.grid.relax_cycles, 3U);
    t::TerrainParams unrelaxed = p;
    unrelaxed.grid.relax_cycles = 0;

    const t::Heightmap noise = synthesize(*g, pass, unrelaxed); // the GPU's own N
    const t::Heightmap gpu = synthesize(*g, pass, p);
    ASSERT_TRUE(noise.valid());
    ASSERT_TRUE(gpu.valid());

    std::vector<float> cpu = noise.heights;
    const std::uint32_t res = p.resolution;
    ASSERT_TRUE(t::erode_grid(cpu, res, noise.metres_per_texel(), p.grid));

    int compared = 0;
    int mismatches = 0;
    double moved = 0.0;
    for (std::uint32_t z = 0; z < res; ++z) {
        for (std::uint32_t x = 0; x < res; ++x) {
            const std::size_t i = static_cast<std::size_t>(z) * res + x;
            const float fade = t::grid_erosion_fade(x, z, res);
            float expected = noise.heights[i];
            if (fade >= 1.0F) {
                const float d = cpu[i] - noise.heights[i];
                expected = noise.heights[i] + d;
            } else if (fade > 0.0F) {
                continue; // the blend may fuse on the GPU: not the relaxation's bits
            }
            ++compared;
            mismatches += same_bits(gpu.heights[i], expected) ? 0 : 1;
            moved = std::max(moved, static_cast<double>(std::abs(cpu[i] - noise.heights[i])));
        }
    }
    std::printf("[terrain synth] talus relaxation: %d of %d texels differ from the CPU "
                "reference; it moved up to %.3g m\n",
                mismatches, compared, moved);
    ASSERT_GT(moved, 1.0) << "the relaxation did nothing";
    EXPECT_EQ(mismatches, 0) << "the GPU cascade's bits differ from run_talus_op's";
}

TEST(GpuTerrainSynth, AdjacentGpuTilesShareBitIdenticalEdges) {
    QUAT_REQUIRE_GPU(g);
    r::TerrainSynthPass pass = make_pass(*g);
    ASSERT_TRUE(pass.valid());

    const t::TerrainParams base = eroded_params(); // erosion on: the fade must hold the edges
    std::map<std::uint64_t, glm::ivec2> ids;
    for (const glm::ivec2 tile : {glm::ivec2(0, 0), glm::ivec2(1, 0), glm::ivec2(0, 1),
                                  glm::ivec2(-1, 0)}) {
        t::TerrainParams p = base;
        p.world_tile = tile;
        ids.emplace(pass.enqueue(p), tile);
    }
    std::map<glm::ivec2, t::Heightmap, bool (*)(const glm::ivec2&, const glm::ivec2&)> maps(
        [](const glm::ivec2& a, const glm::ivec2& b) {
            return a.x != b.x ? a.x < b.x : a.y < b.y;
        });
    for (auto& [id, res] : run_to_completion(*g, pass)) {
        ASSERT_TRUE(res.heights.valid());
        maps.emplace(ids.at(id), std::move(res.heights));
    }
    ASSERT_EQ(maps.size(), 4U);

    const std::uint32_t n = base.resolution;
    const auto at = [&](const glm::ivec2& tile, std::uint32_t x, std::uint32_t z) {
        return maps.at(tile).heights[static_cast<std::size_t>(z) * n + x];
    };
    int mismatches = 0;
    for (std::uint32_t i = 0; i < n; ++i) {
        // +x edge of (0,0) / -x edge of (1,0); -x edge of (0,0) / +x edge of (-1,0).
        mismatches += same_bits(at({0, 0}, n - 1, i), at({1, 0}, 0, i)) ? 0 : 1;
        mismatches += same_bits(at({0, 0}, 0, i), at({-1, 0}, n - 1, i)) ? 0 : 1;
        // +z edge of (0,0) / -z edge of (0,1).
        mismatches += same_bits(at({0, 0}, i, n - 1), at({0, 1}, i, 0)) ? 0 : 1;
    }
    EXPECT_EQ(mismatches, 0) << "GPU tiles diverge along shared edges";
}

TEST(GpuTerrainSynth, RepeatedRunsAndFrameSlicingAreBitIdentical) {
    QUAT_REQUIRE_GPU(g);
    r::TerrainSynthPass pass = make_pass(*g);
    ASSERT_TRUE(pass.valid());

    const t::TerrainParams p = eroded_params();
    const t::Heightmap first = synthesize(*g, pass, p);
    const t::Heightmap second = synthesize(*g, pass, p);
    ASSERT_TRUE(first.valid());
    EXPECT_TRUE(bit_identical(first, second)) << "GPU synthesis is not deterministic";

    // A small budget spreads the simulation over many frames; the result
    // must not depend on where the slices fall.
    pass.set_cell_budget(static_cast<std::uint64_t>(p.resolution) * p.resolution * 7);
    const std::uint64_t id = pass.enqueue(p);
    int frames = 0;
    auto results = run_to_completion(*g, pass, &frames);
    ASSERT_TRUE(results.contains(id));
    EXPECT_GE(frames, static_cast<int>(p.grid.iterations / 7));
    EXPECT_TRUE(bit_identical(first, results.at(id).heights)) << "slicing changed the result";
}

TEST(GpuTerrainSynth, TileImageHoldsTheReadBackHeights) {
    QUAT_REQUIRE_GPU(g);
    r::TerrainSynthPass pass = make_pass(*g);
    ASSERT_TRUE(pass.valid());

    const t::TerrainParams p = noise_params();
    const std::uint64_t id = pass.enqueue(p);
    auto results = run_to_completion(*g, pass);
    ASSERT_TRUE(results.contains(id));
    r::TerrainSynthPass::Result& res = results.at(id);
    ASSERT_NE(res.image.handle(), VK_NULL_HANDLE);
    ASSERT_TRUE(res.heights.valid());

    const VkDeviceSize bytes = res.heights.heights.size() * sizeof(float);
    auto readback = g->allocator.create_buffer(
        bytes, VK_BUFFER_USAGE_TRANSFER_DST_BIT, VMA_MEMORY_USAGE_AUTO,
        VMA_ALLOCATION_CREATE_HOST_ACCESS_RANDOM_BIT | VMA_ALLOCATION_CREATE_MAPPED_BIT);
    ASSERT_OK(readback);
    const VkImage image = res.image.handle();
    ASSERT_OK(quat::test::submit_and_wait(g->device, [&](VkCommandBuffer cmd) {
        VkImageMemoryBarrier2 b{};
        b.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER_2;
        b.srcStageMask = VK_PIPELINE_STAGE_2_VERTEX_SHADER_BIT;
        b.dstStageMask = VK_PIPELINE_STAGE_2_COPY_BIT;
        b.dstAccessMask = VK_ACCESS_2_TRANSFER_READ_BIT;
        b.oldLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL; // as the pass leaves it
        b.newLayout = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL;
        b.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
        b.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
        b.image = image;
        b.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
        VkDependencyInfo dep{};
        dep.sType = VK_STRUCTURE_TYPE_DEPENDENCY_INFO;
        dep.imageMemoryBarrierCount = 1;
        dep.pImageMemoryBarriers = &b;
        vkCmdPipelineBarrier2(cmd, &dep);
        VkBufferImageCopy region{};
        region.imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
        region.imageExtent = {p.resolution, p.resolution, 1};
        vkCmdCopyImageToBuffer(cmd, image, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
                               readback->handle(), 1, &region);
        quat::test::transfer_to_host_barrier(cmd);
    }));
    ASSERT_OK(readback->invalidate_mapped());
    EXPECT_EQ(std::memcmp(readback->mapped(), res.heights.heights.data(), bytes), 0)
        << "the sampled tile image differs from the read-back heights";
}

// The ten kernels are hot-reloadable pipelines whose bindings come from
// reflection by name: reloading the unchanged shader passes the R2
// compatibility check, rebuilds all ten, and synthesis still matches.
TEST(GpuTerrainSynth, HotReloadRebuildsEveryKernel) {
    QUAT_REQUIRE_GPU(g);
    r::TerrainSynthPass pass = make_pass(*g);
    ASSERT_TRUE(pass.valid());
    const t::TerrainParams p = eroded_params();
    const t::Heightmap before = synthesize(*g, pass, p);

    auto report = g->cache.reload(g->shader_dir + "/terrain_synth.spv");
    ASSERT_OK(report);
    EXPECT_TRUE(report->errors.empty()) << report->errors.front();
    EXPECT_GE(report->rebuilt, 10U);
    const t::Heightmap after = synthesize(*g, pass, p);
    ASSERT_TRUE(after.valid());
    EXPECT_TRUE(bit_identical(before, after)) << "the reloaded kernels changed the result";
}

TEST(GpuTerrainSynth, DropletErosionStaysOnTheCpu) {
    QUAT_REQUIRE_GPU(g);
    r::TerrainSynthPass pass = make_pass(*g);
    ASSERT_TRUE(pass.valid());
    t::TerrainParams p = noise_params();
    p.erosion_droplets = 1000;
    EXPECT_FALSE(pass.supports(p));
    EXPECT_EQ(pass.enqueue(p), 0U);
    p.resolution = r::TerrainSynthPass::max_resolution + 2;
    p.erosion_droplets = 0;
    EXPECT_FALSE(pass.supports(p));
    EXPECT_TRUE(pass.idle());
}

// Timing smoke test: logs GPU (wall clock incl. submit + readback) vs CPU
// generation for a default-sized tile. No threshold — lavapipe runs the
// "GPU" on the CPU.
TEST(GpuTerrainSynth, PerformanceSmoke) {
    QUAT_REQUIRE_GPU(g);
    r::TerrainSynthPass pass = make_pass(*g);
    ASSERT_TRUE(pass.valid());

    t::TerrainParams p;
    p.resolution = 513;
    p.tile_size_m = 1000.0F;
    p.erosion_model = t::ErosionModel::grid;
    p.grid.iterations = 50;
    using clock = std::chrono::steady_clock;
    const auto ms = [](clock::duration d) {
        return std::chrono::duration<double, std::milli>(d).count();
    };

    (void)synthesize(*g, pass, p); // warm-up: pipelines, allocations
    const auto g0 = clock::now();
    const t::Heightmap gpu = synthesize(*g, pass, p);
    const auto g1 = clock::now();
    const t::Heightmap cpu = t::generate(p);
    const auto c1 = clock::now();
    ASSERT_TRUE(gpu.valid());
    ASSERT_TRUE(cpu.valid());
    std::printf("[terrain synth] %u² tile, %u erosion steps: GPU %.1f ms, CPU %.1f ms (%s)\n",
                p.resolution, p.grid.iterations, ms(g1 - g0), ms(c1 - g1),
                g->device.properties().deviceName);
}

} // namespace
