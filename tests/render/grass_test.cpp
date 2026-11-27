// GPU grass (R9.3): the grass_generate compute pass read back and compared
// with the CPU reference (grass.hpp), and the draw pass's GBuffer output.

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <map>
#include <span>
#include <utility>
#include <vector>

#include <glm/glm.hpp>
#include <glm/gtc/matrix_transform.hpp>
#include <gtest/gtest.h>
#include <vulkan/vulkan.h>

#include "engine/renderer/grass.hpp"
#include "engine/renderer/grass_pass.hpp"
#include "engine/renderer/mesh_pass.hpp"
#include "engine/renderer/terrain_pass.hpp"
#include "engine/rhi/device.hpp"
#include "engine/rhi/gpu_allocator.hpp"
#include "engine/rhi/render_graph.hpp"
#include "engine/terrain/generator.hpp"
#include "support/expect_ok.hpp"
#include "support/gpu_context.hpp"
#include "support/gpu_submit.hpp"

namespace {

namespace r = engine::renderer;
namespace rhi = engine::rhi;
using engine::core::Error;

// What one generation produced.
struct Generated {
    r::GrassArgsGpu                 args{};
    std::vector<r::GrassBladeGpu>   blades; // near list, then far list (visible only)
    r::GrassCapacity                capacity{};
};

[[nodiscard]] std::expected<rhi::GpuBuffer, Error> readback_buffer(rhi::GpuAllocator& allocator,
                                                                   VkDeviceSize bytes) {
    return allocator.create_buffer(
        bytes, VK_BUFFER_USAGE_TRANSFER_DST_BIT, VMA_MEMORY_USAGE_AUTO,
        VMA_ALLOCATION_CREATE_HOST_ACCESS_RANDOM_BIT | VMA_ALLOCATION_CREATE_MAPPED_BIT);
}

// Runs generation only and reads the lists + arguments back through a graph
// transfer pass (exercising the compute -> transfer buffer barriers).
[[nodiscard]] std::expected<Generated, Error> generate(quat::test::GpuContext& g,
                                                       r::GrassPass& pass,
                                                       const r::GrassFrame& frame) {
    rhi::RenderGraph graph;
    auto out = pass.add_to_graph(graph, 0, frame, nullptr, {});
    if (!out) return std::unexpected(out.error());
    if (!out->valid()) return Generated{};
    const r::GrassOutput o = *out;
    const VkDeviceSize blade_bytes =
        static_cast<VkDeviceSize>(o.capacity.near + o.capacity.far) * sizeof(r::GrassBladeGpu);
    auto args = readback_buffer(g.allocator, sizeof(r::GrassArgsGpu));
    if (!args) return std::unexpected(args.error());
    auto blades = readback_buffer(g.allocator, blade_bytes);
    if (!blades) return std::unexpected(blades.error());
    const VkBuffer args_dst = args->handle();
    const VkBuffer blades_dst = blades->handle();
    (void)graph.add_pass("readback", rhi::PassType::transfer)
        .reads(o.args, rhi::ResourceUsage::transfer_src)
        .reads(o.blades, rhi::ResourceUsage::transfer_src)
        .execute([=](rhi::PassContext& ctx) {
            const VkBufferCopy a{0, 0, sizeof(r::GrassArgsGpu)};
            vkCmdCopyBuffer(ctx.cmd(), ctx.resolve(o.args).buffer, args_dst, 1, &a);
            const VkBufferCopy b{0, 0, blade_bytes};
            vkCmdCopyBuffer(ctx.cmd(), ctx.resolve(o.blades).buffer, blades_dst, 1, &b);
        });
    if (auto c = graph.compile(); !c) return std::unexpected(c.error());
    auto done = quat::test::submit_and_wait(g.device, [&](VkCommandBuffer cmd) {
        graph.execute(cmd);
        quat::test::transfer_to_host_barrier(cmd);
    });
    if (!done) return std::unexpected(done.error());

    Generated gen;
    gen.capacity = o.capacity;
    std::memcpy(&gen.args, args->mapped(), sizeof(gen.args));
    const auto* all = static_cast<const r::GrassBladeGpu*>(blades->mapped());
    const std::uint32_t near = gen.args.near.instanceCount;
    const std::uint32_t far = gen.args.far.instanceCount;
    gen.blades.assign(all, all + near);
    // Far blades start at the near region's capacity; the indirect commands
    // keep firstInstance = 0 (drawIndirectFirstInstance is not required) and
    // the draw pass passes the region base as a push constant.
    gen.blades.insert(gen.blades.end(), all + gen.capacity.near, all + gen.capacity.near + far);
    return gen;
}

glm::mat4 vulkan_perspective(float fov_y, float aspect, float near_z, float far_z) {
    glm::mat4 p = glm::perspective(fov_y, aspect, near_z, far_z);
    p[1][1] *= -1.0F;
    return p;
}

// A top-down orthographic view that contains the whole grass disc (culling
// then removes nothing).
glm::mat4 overview(const glm::vec3& centre) {
    const glm::mat4 view = glm::lookAt(centre + glm::vec3(0.0F, 100.0F, 0.0F), centre,
                                       glm::vec3(0.0F, 0.0F, 1.0F));
    return glm::ortho(-120.0F, 120.0F, -120.0F, 120.0F, 1.0F, 300.0F) * view;
}

// Flat ground at y = 0 everywhere, keep every candidate inside the radius
// (no thinning; only the outer edge band fades).
r::GrassSettings flat_settings() {
    r::GrassSettings s;
    s.seed = 77;
    s.density = 20.0F;
    s.radius = 20.0F;
    s.terrain_only = false;
    s.ground_y = 0.0F;
    s.lod_start = 1.0F;
    s.far_density = 1.0F;
    return s;
}

// The CPU reference: how many candidates the settings place around `camera`
// on flat ground at y = 0.
std::uint32_t reference_placed(const r::GrassSettings& s, const glm::vec3& camera) {
    const r::GrassGrid grid = r::grass_grid(s, camera);
    std::uint32_t placed = 0;
    for (std::uint32_t z = 0; z < grid.cells.y; ++z) {
        for (std::uint32_t x = 0; x < grid.cells.x; ++x) {
            const glm::ivec2 cell = grid.origin + glm::ivec2(x, z);
            const r::GrassCandidate c = r::grass_candidate(s, grid.cell_size, cell);
            if (glm::length(c.root_xz - glm::vec2(camera.x, camera.z)) >= s.radius) continue;
            const float d = glm::length(glm::vec3(c.root_xz.x, 0.0F, c.root_xz.y) - camera);
            if (r::grass_blade_fade(r::grass_keep_fraction(s, d), c.threshold) > 0.0F) ++placed;
        }
    }
    return placed;
}

TEST(GpuGrass, GeneratedCountMatchesDensityTimesArea) {
    QUAT_REQUIRE_GPU(g);
    auto pass = r::GrassPass::create(g->device, g->allocator, g->cache, g->transfer, g->shader_dir, 1);
    ASSERT_OK(pass);

    const r::GrassSettings s = flat_settings();
    const glm::vec3 camera(3.3F, 0.0F, -2.7F);
    r::GrassFrame frame;
    frame.settings = s;
    frame.camera = camera;
    frame.draw_view_proj = frame.cull_view_proj = overview(camera);
    auto gen = generate(*g, *pass, frame);
    ASSERT_OK(gen);

    // Both indirect commands draw from instance 0: a non-zero firstInstance
    // would need drawIndirectFirstInstance, which the engine doesn't enable
    // (PR #3 review). The far region's base is a push constant instead.
    EXPECT_EQ(gen->args.near.firstInstance, 0U);
    EXPECT_EQ(gen->args.far.firstInstance, 0U);

    // Every cell within the radius is a candidate; all placed blades are visible.
    const float area = 3.14159265F * s.radius * s.radius;
    EXPECT_NEAR(static_cast<float>(gen->args.candidates), s.density * area, s.density * area * 0.01F);
    EXPECT_EQ(gen->args.placed, gen->args.near_count + gen->args.far_count);
    EXPECT_EQ(gen->blades.size(), gen->args.placed);

    // Placed = density x the LOD-weighted area (only the edge band thins).
    double weighted = 0.0;
    constexpr int k_rings = 4000;
    for (int i = 0; i < k_rings; ++i) {
        const float d = (static_cast<float>(i) + 0.5F) * s.radius / k_rings;
        const float fade_ok = r::grass_keep_fraction(s, d); // P(kept) for a uniform threshold
        weighted += 2.0 * 3.14159265 * d * (s.radius / k_rings) *
                    std::min(1.0, static_cast<double>(fade_ok) / (1.0 - r::grass_fade_band));
    }
    EXPECT_NEAR(static_cast<double>(gen->args.placed), s.density * weighted, s.density * weighted * 0.02);
    // ...and the GPU agrees with the CPU reference cell by cell (to rounding).
    const std::uint32_t reference = reference_placed(s, camera);
    EXPECT_NEAR(static_cast<double>(gen->args.placed), reference, reference * 0.002 + 2.0);

    // With thinning enabled the counts still follow the reference.
    frame.settings.lod_start = 0.3F;
    frame.settings.far_density = 0.2F;
    auto thinned = generate(*g, *pass, frame);
    ASSERT_OK(thinned);
    const std::uint32_t thinned_ref = reference_placed(frame.settings, camera);
    EXPECT_NEAR(static_cast<double>(thinned->args.placed), thinned_ref, thinned_ref * 0.002 + 2.0);
    EXPECT_LT(thinned->args.placed, gen->args.placed * 3 / 4);
    EXPECT_GT(thinned->args.near_count, 0U);
    EXPECT_GT(thinned->args.far_count, 0U);
    // The far LOD path is exercised here: its command still starts at
    // instance 0, and the far blades read from the near capacity onwards
    // are real, placed blades (finite root, non-zero packed shape/fade).
    EXPECT_EQ(thinned->args.far.firstInstance, 0U);
    ASSERT_GT(thinned->args.far.instanceCount, 0U);
    ASSERT_EQ(thinned->blades.size(),
              std::size_t{thinned->args.near.instanceCount} + thinned->args.far.instanceCount);
    const r::GrassBladeGpu& first_far = thinned->blades[thinned->args.near.instanceCount];
    EXPECT_TRUE(std::isfinite(first_far.root.x) && std::isfinite(first_far.root.z));
    EXPECT_NE(first_far.packed, 0U);
}

TEST(GpuGrass, BladesStayFixedInTheWorldAsTheCameraMoves) {
    QUAT_REQUIRE_GPU(g);
    auto pass = r::GrassPass::create(g->device, g->allocator, g->cache, g->transfer, g->shader_dir, 1);
    ASSERT_OK(pass);

    r::GrassFrame frame;
    frame.settings = flat_settings();
    const glm::vec3 a(0.4F, 0.0F, 0.2F);
    const glm::vec3 b = a + glm::vec3(1.37F, 0.0F, 0.71F); // not a whole number of cells
    const auto run = [&](const glm::vec3& camera) {
        frame.camera = camera;
        frame.draw_view_proj = frame.cull_view_proj = overview(camera);
        frame.time_s = camera.x; // the wind clock must not matter either
        return generate(*g, *pass, frame);
    };
    auto first = run(a);
    ASSERT_OK(first);
    auto second = run(b);
    ASSERT_OK(second);

    // Blades well inside both discs (no edge fade in either frame) must be
    // bit-identical in both frames.
    const float inner = frame.settings.radius * 0.8F;
    const auto key = [](const r::GrassBladeGpu& bl) {
        std::uint64_t k = 0;
        std::memcpy(&k, &bl.root.x, 4);
        std::uint32_t z = 0;
        std::memcpy(&z, &bl.root.z, 4);
        return (k << 32) | z;
    };
    const auto inside_both = [&](const r::GrassBladeGpu& bl) {
        return glm::length(bl.root - a) < inner && glm::length(bl.root - b) < inner;
    };
    std::map<std::uint64_t, r::GrassBladeGpu> from_first;
    for (const auto& bl : first->blades) {
        if (inside_both(bl)) from_first.emplace(key(bl), bl);
    }
    std::size_t matched = 0;
    for (const auto& bl : second->blades) {
        if (!inside_both(bl)) continue;
        const auto it = from_first.find(key(bl));
        ASSERT_NE(it, from_first.end()) << "blade at " << bl.root.x << ", " << bl.root.z
                                        << " appeared only after the camera moved";
        EXPECT_EQ(it->second.root.y, bl.root.y);
        EXPECT_EQ(it->second.packed, bl.packed);
        ++matched;
    }
    EXPECT_EQ(matched, from_first.size());
    EXPECT_GT(matched, 5000U);

    // The GPU positions are the CPU reference's.
    const float cell = r::grass_cell_size(frame.settings);
    for (std::size_t i = 0; i < second->blades.size(); i += 97) {
        const r::GrassBladeGpu& bl = second->blades[i];
        const glm::ivec2 c(static_cast<int>(std::floor(bl.root.x / cell)),
                           static_cast<int>(std::floor(bl.root.z / cell)));
        const r::GrassCandidate ref = r::grass_candidate(frame.settings, cell, c);
        EXPECT_NEAR(ref.root_xz.x, bl.root.x, 1e-4F);
        EXPECT_NEAR(ref.root_xz.y, bl.root.z, 1e-4F);
        EXPECT_EQ(ref.hash, bl.packed & 0xFFFFU);
    }
}

TEST(GpuGrass, FrustumCullingRemovesOffscreenBlades) {
    QUAT_REQUIRE_GPU(g);
    auto pass = r::GrassPass::create(g->device, g->allocator, g->cache, g->transfer, g->shader_dir, 1);
    ASSERT_OK(pass);

    r::GrassFrame frame;
    frame.settings = flat_settings();
    frame.camera = glm::vec3(0.0F, 1.7F, 0.0F);
    frame.draw_view_proj = frame.cull_view_proj = overview(frame.camera);
    auto everything = generate(*g, *pass, frame);
    ASSERT_OK(everything);

    const glm::mat4 view = glm::lookAt(frame.camera, glm::vec3(10.0F, 0.0F, 0.0F),
                                       glm::vec3(0.0F, 1.0F, 0.0F));
    frame.cull_view_proj = vulkan_perspective(glm::radians(60.0F), 1.0F, 0.1F, 200.0F) * view;
    frame.draw_view_proj = frame.cull_view_proj;
    auto gen = generate(*g, *pass, frame);
    ASSERT_OK(gen);

    const std::uint32_t visible = gen->args.near_count + gen->args.far_count;
    EXPECT_GT(visible, 100U);
    EXPECT_LT(visible, everything->args.placed / 4); // a 60-degree view of a full disc
    // Whole tiles outside the view were skipped before any per-blade work.
    EXPECT_LT(gen->args.candidates, everything->args.candidates / 2);
    const auto planes = r::grass_frustum_planes(frame.cull_view_proj);
    const float max_h = frame.settings.height_range.y;
    for (const auto& bl : gen->blades) {
        EXPECT_GT(bl.root.x, -max_h) << "blade behind the camera";
        const glm::vec3 c = bl.root + glm::vec3(0.0F, 0.5F * max_h, 0.0F);
        for (const glm::vec4& p : planes) {
            ASSERT_GE(glm::dot(glm::vec3(p), c) + p.w, -max_h - 1e-3F) << "blade outside the frustum";
        }
    }
}

// A 64 m tile: flat at 0 m for x < 0, a 45-degree ramp up to 8 m, then an
// 8 m plateau.
struct RampTile {
    rhi::GpuImage  image;
    rhi::ImageView view;
    r::TerrainHeightTile tile;
};

std::expected<RampTile, Error> ramp_tile(quat::test::GpuContext& g) {
    constexpr std::uint32_t res = 65;
    constexpr float size = 64.0F;
    std::vector<float> heights(res * res);
    for (std::uint32_t z = 0; z < res; ++z) {
        for (std::uint32_t x = 0; x < res; ++x) {
            const float wx = -32.0F + static_cast<float>(x) * size / (res - 1);
            heights[z * res + x] = std::clamp(wx, 0.0F, 8.0F);
        }
    }
    RampTile t;
    auto image = rhi::upload_device_image(g.allocator, g.transfer, heights.data(),
                                          heights.size() * sizeof(float), {res, res},
                                          VK_FORMAT_R32_SFLOAT);
    if (!image) return std::unexpected(image.error());
    t.image = std::move(*image);
    auto view = rhi::create_image_view(g.device.handle(), t.image.handle(), VK_FORMAT_R32_SFLOAT);
    if (!view) return std::unexpected(view.error());
    t.view = std::move(*view);
    t.tile.view = t.view.handle();
    t.tile.origin_xz = glm::vec2(-32.0F);
    t.tile.size_m = size;
    t.tile.base_y = 0.0F;
    t.tile.min_height = 0.0F;
    t.tile.max_height = 8.0F;
    t.tile.resolution = res;
    return t;
}

TEST(GpuGrass, SlopeAndHeightLimitsRejectBlades) {
    QUAT_REQUIRE_GPU(g);
    auto pass = r::GrassPass::create(g->device, g->allocator, g->cache, g->transfer, g->shader_dir, 1);
    ASSERT_OK(pass);
    auto tile = ramp_tile(*g);
    ASSERT_OK(tile);

    r::GrassFrame frame;
    frame.settings = flat_settings();
    frame.settings.radius = 40.0F; // reaches past the tile on every side
    frame.settings.terrain_only = true;
    frame.camera = glm::vec3(0.0F, 2.0F, 0.0F);
    frame.draw_view_proj = frame.cull_view_proj = overview(frame.camera);
    const std::array<r::TerrainHeightTile, 1> tiles{tile->tile};
    frame.terrain = tiles;

    // Slopes up to 30 degrees: the 45-degree ramp is bare.
    frame.settings.slope_deg = glm::vec2(0.0F, 30.0F);
    auto no_ramp = generate(*g, *pass, frame);
    ASSERT_OK(no_ramp);
    std::size_t low = 0;
    std::size_t plateau = 0;
    for (const auto& bl : no_ramp->blades) {
        ASSERT_LE(std::abs(bl.root.x), 32.0F) << "blade off the terrain";
        ASSERT_LE(std::abs(bl.root.z), 32.0F) << "blade off the terrain";
        ASSERT_FALSE(bl.root.x > 1.0F && bl.root.x < 7.0F) << "blade on the ramp at x " << bl.root.x;
        if (bl.root.x < -1.0F) {
            ++low;
            EXPECT_NEAR(bl.root.y, 0.0F, 1e-3F);
        }
        if (bl.root.x > 9.0F) {
            ++plateau;
            EXPECT_NEAR(bl.root.y, 8.0F, 1e-3F);
        }
    }
    EXPECT_GT(low, 1000U);
    EXPECT_GT(plateau, 1000U);

    // Heights up to 5 m: the plateau is bare too.
    frame.settings.height_m = glm::vec2(-1.0F, 5.0F);
    auto low_only = generate(*g, *pass, frame);
    ASSERT_OK(low_only);
    for (const auto& bl : low_only->blades) ASSERT_LT(bl.root.x, 1.0F);
    EXPECT_GT(low_only->blades.size(), 1000U);

    // Terrain-only grass without terrain grows nowhere.
    frame.terrain = {};
    rhi::RenderGraph graph;
    auto none = pass->add_to_graph(graph, 0, frame, nullptr, {});
    ASSERT_OK(none);
    EXPECT_FALSE(none->valid());
}

TEST(GpuGrass, DrawWritesBladesIntoTheGBuffer) {
    QUAT_REQUIRE_GPU(g);
    auto mesh = r::MeshPass::create(g->device, g->allocator, g->cache, g->transfer, g->shader_dir);
    ASSERT_OK(mesh);
    auto pass = r::GrassPass::create(g->device, g->allocator, g->cache, g->transfer, g->shader_dir, 2);
    ASSERT_OK(pass);

    constexpr VkExtent2D extent{128, 128};
    r::GrassFrame frame;
    frame.settings = r::GrassSettings{};
    frame.settings.terrain_only = false;
    frame.camera = glm::vec3(0.0F, 1.2F, 0.0F);
    const glm::mat4 view = glm::lookAt(frame.camera, glm::vec3(10.0F, 0.0F, 0.0F),
                                       glm::vec3(0.0F, 1.0F, 0.0F));
    frame.cull_view_proj = vulkan_perspective(glm::radians(70.0F), 1.0F, 0.1F, 200.0F) * view;
    frame.draw_view_proj = frame.cull_view_proj;
    frame.time_s = 2.0F;

    rhi::TransientImagePool pool(g->device, g->allocator);
    rhi::RenderGraph graph(pool);
    auto gbuffer = mesh->add_to_graph(graph, extent, frame.draw_view_proj, r::InstancedDrawList{});
    ASSERT_OK(gbuffer);
    auto grass = pass->add_to_graph(graph, 1, frame, &*gbuffer, extent);
    ASSERT_OK(grass);
    ASSERT_TRUE(grass->valid());

    const VkDeviceSize bytes = static_cast<VkDeviceSize>(extent.width) * extent.height * 4;
    auto readback = readback_buffer(g->allocator, bytes);
    ASSERT_OK(readback);
    const VkBuffer dst = readback->handle();
    const r::GBufferTargets t = *gbuffer;
    (void)graph.add_pass("readback", rhi::PassType::transfer)
        .reads(t.albedo, rhi::ResourceUsage::transfer_src)
        .execute([=](rhi::PassContext& ctx) {
            VkBufferImageCopy region{};
            region.imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
            region.imageExtent = {extent.width, extent.height, 1};
            vkCmdCopyImageToBuffer(ctx.cmd(), ctx.resolve(t.albedo).image,
                                   VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, dst, 1, &region);
        });
    ASSERT_OK(graph.compile());
    ASSERT_OK(quat::test::submit_and_wait(g->device, [&](VkCommandBuffer cmd) {
        graph.execute(cmd);
        quat::test::transfer_to_host_barrier(cmd);
    }));

    // The ground half of the image is carpeted with green blades; the rows
    // well above the horizon stay clear.
    const auto* px = static_cast<const std::uint8_t*>(readback->mapped());
    std::size_t ground = 0;
    std::size_t ground_covered = 0;
    std::size_t sky_covered = 0;
    std::uint64_t r_sum = 0;
    std::uint64_t g_sum = 0;
    for (std::uint32_t y = 0; y < extent.height; ++y) {
        for (std::uint32_t x = 0; x < extent.width; ++x) {
            const std::uint8_t* p = px + (static_cast<std::size_t>(y) * extent.width + x) * 4;
            const bool covered = p[3] > 200;
            if (y >= extent.height * 3 / 4) {
                ++ground;
                if (covered) {
                    ++ground_covered;
                    r_sum += p[0];
                    g_sum += p[1];
                }
            } else if (y < extent.height / 4 && covered) {
                ++sky_covered;
            }
        }
    }
    EXPECT_GT(ground_covered, ground * 6 / 10);
    EXPECT_EQ(sky_covered, 0U);
    EXPECT_GT(g_sum, r_sum) << "blades are not green";
}

// The terrain pass's tiles through TerrainHeightTile: blades stand on the
// surface the terrain pass renders (its uv convention: uv = local / size,
// bilinear over texel centres), only inside the loaded tiles.
TEST(GpuGrass, GrowsOnTheTerrainPassSurface) {
    QUAT_REQUIRE_GPU(g);
    auto terrain = r::TerrainPass::create(g->device, g->allocator, g->cache, g->transfer, g->shader_dir);
    ASSERT_OK(terrain);
    auto pass = r::GrassPass::create(g->device, g->allocator, g->cache, g->transfer, g->shader_dir, 1);
    ASSERT_OK(pass);

    engine::terrain::TerrainParams tp;
    tp.resolution = 129;
    tp.tile_size_m = 128.0F;
    tp.height_m = 20.0F;
    tp.octaves = 4;
    tp.erosion_droplets = 0;
    const engine::terrain::Heightmap map = engine::terrain::generate(tp);
    ASSERT_OK(terrain->set_tile(glm::ivec2(0, 0), map, g->transfer));
    const glm::vec3 anchor(10.0F, -5.0F, -20.0F); // tile (0,0) is centred here

    r::GrassFrame frame;
    frame.settings = flat_settings();
    frame.settings.radius = 30.0F;
    frame.settings.slope_deg = glm::vec2(0.0F, 90.0F);
    frame.settings.terrain_only = true;
    frame.camera = anchor + glm::vec3(50.0F, 30.0F, 50.0F); // the disc overhangs the tile corner
    frame.draw_view_proj = frame.cull_view_proj = overview(frame.camera);
    const auto tiles = terrain->height_tiles(glm::vec2(frame.camera.x, frame.camera.z),
                                             frame.settings.radius, anchor,
                                             r::grass_max_height_tiles);
    ASSERT_EQ(tiles.size(), 1U);
    EXPECT_EQ(tiles[0].origin_xz, glm::vec2(anchor.x - 64.0F, anchor.z - 64.0F));
    EXPECT_TRUE(terrain->height_tiles(glm::vec2(500.0F), 30.0F, anchor, 4).empty());
    frame.terrain = tiles;
    auto gen = generate(*g, *pass, frame);
    ASSERT_OK(gen);
    ASSERT_GT(gen->blades.size(), 1000U);

    // Bilinear over texel centres at uv = local / size (clamped), as sampled.
    const auto rendered_height = [&](glm::vec2 xz) {
        const glm::vec2 uv = (xz - tiles[0].origin_xz) / tiles[0].size_m;
        const glm::vec2 t = glm::clamp(uv * static_cast<float>(map.resolution) - 0.5F,
                                       glm::vec2(0.0F),
                                       glm::vec2(static_cast<float>(map.resolution - 1)));
        const glm::ivec2 i0(t);
        const glm::vec2 f = t - glm::vec2(i0);
        const float h00 = map.at(i0.x, i0.y);
        const float h10 = map.at(i0.x + 1, i0.y);
        const float h01 = map.at(i0.x, i0.y + 1);
        const float h11 = map.at(i0.x + 1, i0.y + 1);
        return anchor.y + glm::mix(glm::mix(h00, h10, f.x), glm::mix(h01, h11, f.x), f.y);
    };
    for (const auto& bl : gen->blades) {
        const glm::vec2 local = glm::vec2(bl.root.x, bl.root.z) - tiles[0].origin_xz;
        ASSERT_TRUE(local.x >= 0.0F && local.y >= 0.0F && local.x <= 128.0F && local.y <= 128.0F)
            << "blade off the tile at " << bl.root.x << ", " << bl.root.z;
        // Linear filtering runs at reduced (8-bit) sub-texel precision on GPUs.
        ASSERT_NEAR(bl.root.y, rendered_height(glm::vec2(bl.root.x, bl.root.z)), 0.05F);
    }
}

} // namespace
