// Terrain edits on the GPU (G19.1): an R10.2 synthesis job with stamps
// against the CPU reference (terrain::apply_edits) on the very heights the
// GPU edited — its read-back generated field — so only the edit arithmetic
// is compared. GPU sqrt and fused multiply-adds differ by an ULP or two from
// the CPU, so heights agree within k_height_tolerance_m and paint channels
// within one 8-bit step; the paint image and the tile tags are checked too.

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <map>
#include <vector>

#include <glm/glm.hpp>
#include <gtest/gtest.h>
#include <vulkan/vulkan.h>

#include "engine/renderer/terrain_synth_pass.hpp"
#include "engine/rhi/device.hpp"
#include "engine/rhi/render_graph.hpp"
#include "engine/terrain/edits.hpp"
#include "engine/terrain/generator.hpp"
#include "support/expect_ok.hpp"
#include "support/gpu_context.hpp"
#include "support/gpu_submit.hpp"

namespace {

namespace r = engine::renderer;
namespace rhi = engine::rhi;
namespace t = engine::terrain;

constexpr float k_height_tolerance_m = 2e-3F;

[[nodiscard]] t::TerrainParams tile_params(glm::ivec2 tile) {
    t::TerrainParams p;
    p.resolution = 129;
    p.tile_size_m = 256.0F;
    p.height_m = 40.0F;
    p.erosion_droplets = 0;
    p.world_tile = tile;
    return p;
}

// Every op and shape, smoothing between pointwise runs, a stamp across the
// tile border, a tilted plane.
[[nodiscard]] std::vector<t::TerrainStamp> stamps() {
    t::TerrainEditList list;
    t::TerrainStamp plinth;
    plinth.shape = t::StampShape::polygon;
    plinth.points = {{-30, -20}, {20, -25}, {25, 15}, {-10, 30}, {-35, 5}};
    plinth.radius = 2.0F;
    plinth.falloff = 6.0F;
    plinth.op = t::StampOp::flatten;
    plinth.height = 12.0F;
    (void)list.add(plinth);
    t::TerrainStamp path;
    path.shape = t::StampShape::capsule;
    path.points = {{-120, 60}, {-40, 50}, {10, 70}, {90, 40}, {140, 60}, {220, 90}}; // crosses x = 128
    path.radius = 2.5F;
    path.falloff = 3.0F;
    path.op = t::StampOp::paint;
    path.channel = t::PaintChannel::dirt;
    (void)list.add(path);
    t::TerrainStamp mask = path;
    mask.channel = t::PaintChannel::grass;
    mask.value = 0.0F;
    (void)list.add(mask);
    t::TerrainStamp blur = path;
    blur.op = t::StampOp::smooth;
    blur.iterations = 2;
    blur.strength = 0.7F;
    (void)list.add(blur);
    t::TerrainStamp mound;
    mound.points = {{60, -60}};
    mound.radius = 8.0F;
    mound.falloff = 12.0F;
    mound.op = t::StampOp::raise;
    mound.amount = 5.0F;
    (void)list.add(mound);
    t::TerrainStamp dig = mound;
    dig.op = t::StampOp::lower;
    dig.amount = 2.0F;
    dig.strength = 0.5F;
    (void)list.add(dig);
    t::TerrainStamp ramp;
    ramp.points = {{-70, -70}};
    ramp.radius = 15.0F;
    ramp.falloff = 5.0F;
    ramp.op = t::StampOp::flatten;
    ramp.normal = glm::vec3(0.15F, 1.0F, -0.1F);
    ramp.pivot = {-70, -70};
    ramp.height = 3.0F;
    (void)list.add(ramp);
    t::TerrainStamp gravel = ramp;
    gravel.op = t::StampOp::paint;
    gravel.channel = t::PaintChannel::gravel;
    gravel.value = 0.6F;
    (void)list.add(gravel);
    return list.stamps();
}

[[nodiscard]] r::TerrainSynthPass::Result synthesize(quat::test::GpuContext& g,
                                                     r::TerrainSynthPass& pass,
                                                     const t::TerrainParams& params,
                                                     std::span<const t::TerrainStamp> list,
                                                     std::uint64_t revision) {
    r::TerrainSynthPass::EditRequest edits;
    edits.packed = t::pack_stamps(list);
    edits.revision = revision;
    const std::uint64_t id = pass.enqueue(params, std::move(edits));
    EXPECT_NE(id, 0U);
    for (int frame = 0; frame < 100 && !pass.idle(); ++frame) {
        rhi::RenderGraph graph;
        EXPECT_OK(pass.add_to_graph(graph));
        EXPECT_OK(graph.compile());
        EXPECT_OK(quat::test::submit_and_wait(g.device, [&](VkCommandBuffer cmd) {
            graph.execute(cmd);
        }));
        for (r::TerrainSynthPass::Result& res : pass.collect()) {
            if (res.id == id) return std::move(res);
        }
    }
    ADD_FAILURE() << "synthesis did not finish";
    return {};
}

TEST(GpuTerrainEdits, MatchTheCpuReference) {
    QUAT_REQUIRE_GPU(g);
    auto created = r::TerrainSynthPass::create(g->device, g->allocator, g->cache, g->shader_dir);
    ASSERT_OK(created);
    r::TerrainSynthPass pass = std::move(*created);
    pass.set_cell_budget(0);
    ASSERT_TRUE(pass.edits_supported());

    const std::vector<t::TerrainStamp> list = stamps();
    for (const glm::ivec2 tile : {glm::ivec2(0, 0), glm::ivec2(1, 0)}) {
        const t::TerrainParams params = tile_params(tile);
        r::TerrainSynthPass::Result res = synthesize(*g, pass, params, list, 42);
        const t::Heightmap& gpu = res.heights;
        ASSERT_TRUE(gpu.valid());
        EXPECT_EQ(gpu.edit_revision, 42U);
        ASSERT_EQ(gpu.base_heights.size(), gpu.heights.size());
        ASSERT_EQ(gpu.paint.size(), gpu.heights.size());
        EXPECT_NE(res.paint.handle(), VkImage{VK_NULL_HANDLE});
        EXPECT_EQ(gpu.edit_cells.size(), t::edit_cells_per_side * t::edit_cells_per_side);

        // The CPU reference on the GPU's own generated heights.
        t::Heightmap cpu;
        cpu.resolution = gpu.resolution;
        cpu.tile_size_m = gpu.tile_size_m;
        cpu.heights = gpu.base_heights;
        (void)t::apply_edits(cpu, t::TileFrame{tile, gpu.resolution, gpu.tile_size_m}, list);

        float max_dh = 0.0F;
        int max_dpaint = 0;
        std::size_t edited = 0;
        for (std::size_t i = 0; i < gpu.heights.size(); ++i) {
            max_dh = std::max(max_dh, std::abs(gpu.heights[i] - cpu.heights[i]));
            if (gpu.heights[i] != gpu.base_heights[i]) ++edited;
            for (std::uint32_t c = 0; c < t::paint_channel_count; ++c) {
                const int a = static_cast<int>((gpu.paint[i] >> (8 * c)) & 0xFF);
                const int b = static_cast<int>((cpu.paint[i] >> (8 * c)) & 0xFF);
                max_dpaint = std::max(max_dpaint, std::abs(a - b));
            }
        }
        std::printf("[terrain edits] tile (%d,%d): %zu texels edited, max |gpu-cpu| %.3g m, "
                    "paint %d step(s)\n",
                    tile.x, tile.y, edited, static_cast<double>(max_dh), max_dpaint);
        EXPECT_GT(edited, 100U);
        EXPECT_LE(max_dh, k_height_tolerance_m);
        EXPECT_LE(max_dpaint, 1);
    }
}

TEST(GpuTerrainEdits, NoStampsLeaveTheTileUnedited) {
    QUAT_REQUIRE_GPU(g);
    auto created = r::TerrainSynthPass::create(g->device, g->allocator, g->cache, g->shader_dir);
    ASSERT_OK(created);
    r::TerrainSynthPass pass = std::move(*created);
    pass.set_cell_budget(0);
    r::TerrainSynthPass::Result res = synthesize(*g, pass, tile_params({0, 0}), {}, 7);
    ASSERT_TRUE(res.heights.valid());
    EXPECT_EQ(res.heights.edit_revision, 7U);
    EXPECT_TRUE(res.heights.base_heights.empty());
    EXPECT_TRUE(res.heights.paint.empty());
    EXPECT_EQ(res.paint.handle(), VkImage{VK_NULL_HANDLE});
}

} // namespace
