// GPU grass v2 (#119): clumped placement against the CPU reference, the
// pixel-footprint LOD, the root tint read from the GBuffer, and the ground
// cover pass.

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <vector>

#include <glm/glm.hpp>
#include <glm/gtc/matrix_transform.hpp>
#include <gtest/gtest.h>
#include <vulkan/vulkan.h>

#include "engine/asset/material_asset.hpp"
#include "engine/asset/mesh_asset.hpp"
#include "engine/renderer/grass.hpp"
#include "engine/renderer/grass_pass.hpp"
#include "engine/renderer/mesh_pass.hpp"
#include "engine/rhi/device.hpp"
#include "engine/rhi/gpu_allocator.hpp"
#include "engine/rhi/render_graph.hpp"
#include "support/expect_ok.hpp"
#include "support/gpu_context.hpp"
#include "support/grass_readback.hpp"
#include "support/render_readback.hpp"

namespace {

namespace r = engine::renderer;
namespace rhi = engine::rhi;
namespace asset = engine::asset;

constexpr VkExtent2D k_extent{128, 128};

glm::mat4 vulkan_perspective(float fov_y, float aspect, float near_z, float far_z) {
    glm::mat4 p = glm::perspective(fov_y, aspect, near_z, far_z);
    p[1][1] *= -1.0F;
    return p;
}

// Flat ground at y = 0, no distance thinning (only the edge band fades).
r::GrassSettings flat_settings() {
    r::GrassSettings s;
    s.seed = 5;
    s.density = 20.0F;
    s.radius = 16.0F;
    s.terrain_only = false;
    s.ground_y = 0.0F;
    s.lod_start = 1.0F;
    s.far_density = 1.0F;
    return s;
}

// The CPU reference count of placed blades around `camera` on flat ground,
// with the pixel-footprint term at `pixels_per_metre` (0: off).
std::uint32_t reference_placed(const r::GrassSettings& s, const glm::vec3& camera,
                               float pixels_per_metre) {
    const r::GrassGrid grid = r::grass_grid(s, camera);
    std::uint32_t placed = 0;
    for (std::uint32_t z = 0; z < grid.cells.y; ++z) {
        for (std::uint32_t x = 0; x < grid.cells.x; ++x) {
            const r::GrassCandidate c =
                r::grass_candidate(s, grid.cell_size, grid.origin + glm::ivec2(x, z));
            if (glm::length(c.root_xz - glm::vec2(camera.x, camera.z)) >= s.radius) continue;
            const float d = glm::length(glm::vec3(c.root_xz.x, 0.0F, c.root_xz.y) - camera);
            const float keep = r::grass_keep_fraction(s, d) *
                               r::grass_footprint_keep(s, d, pixels_per_metre);
            if (r::grass_blade_fade(keep, c.threshold) > 0.0F) ++placed;
        }
    }
    return placed;
}

// A top-down orthographic view that contains the whole disc.
glm::mat4 overview(const glm::vec3& centre) {
    const glm::mat4 view = glm::lookAt(centre + glm::vec3(0.0F, 100.0F, 0.0F), centre,
                                       glm::vec3(0.0F, 0.0F, 1.0F));
    return glm::ortho(-120.0F, 120.0F, -120.0F, 120.0F, 1.0F, 300.0F) * view;
}

asset::Vertex vertex(glm::vec3 p, glm::vec3 n) {
    asset::Vertex v{};
    v.position = p;
    v.normal = n;
    return v;
}

// A 200 m ground quad at y = 0 and a 2 m x 2 m wall facing -X at x = 5
// (z in [-1, 1], y in [0, 2]): two meshes, so each takes its own material.
struct Scene {
    asset::MeshAsset     ground;
    asset::MeshAsset     wall;
    asset::MaterialAsset ground_material;
    asset::MaterialAsset wall_material;
};

std::expected<Scene, engine::core::Error> make_scene(quat::test::GpuContext& g,
                                                     glm::vec3 ground_albedo,
                                                     glm::vec3 wall_albedo) {
    const glm::vec3 up(0.0F, 1.0F, 0.0F);
    const std::array<asset::Vertex, 4> ground{
        vertex({-100.0F, 0.0F, -100.0F}, up), vertex({100.0F, 0.0F, -100.0F}, up),
        vertex({100.0F, 0.0F, 100.0F}, up), vertex({-100.0F, 0.0F, 100.0F}, up)};
    const glm::vec3 west(-1.0F, 0.0F, 0.0F);
    const std::array<asset::Vertex, 4> wall{
        vertex({5.0F, 0.0F, -1.0F}, west), vertex({5.0F, 0.0F, 1.0F}, west),
        vertex({5.0F, 2.0F, 1.0F}, west), vertex({5.0F, 2.0F, -1.0F}, west)};
    // Both windings: visible whatever the pass's cull mode and front face.
    const std::array<std::uint32_t, 12> quad{0, 1, 2, 0, 2, 3, 0, 2, 1, 0, 3, 2};
    Scene scene;
    auto gm = quat::test::upload_mesh(g, ground, quad);
    if (!gm) return std::unexpected(gm.error());
    scene.ground = std::move(*gm);
    auto wm = quat::test::upload_mesh(g, wall, quad);
    if (!wm) return std::unexpected(wm.error());
    scene.wall = std::move(*wm);
    const auto material = [&](glm::vec3 albedo) {
        asset::PbrMaterialParams params;
        params.base_color_factor = glm::vec4(albedo, 1.0F);
        return asset::upload_material(params, asset::MaterialTextures{}, g.allocator, g.transfer);
    };
    auto gmat = material(ground_albedo);
    if (!gmat) return std::unexpected(gmat.error());
    scene.ground_material = std::move(*gmat);
    auto wmat = material(wall_albedo);
    if (!wmat) return std::unexpected(wmat.error());
    scene.wall_material = std::move(*wmat);
    return scene;
}

// A camera 1.2 m above the ground at the origin looking down +X.
r::GrassFrame ground_level_frame(const glm::vec3& target) {
    r::GrassFrame frame;
    frame.settings = flat_settings();
    frame.camera = glm::vec3(0.0F, 1.2F, 0.0F);
    const glm::mat4 view = glm::lookAt(frame.camera, target, glm::vec3(0.0F, 1.0F, 0.0F));
    frame.cull_view_proj = vulkan_perspective(glm::radians(60.0F), 1.0F, 0.1F, 300.0F) * view;
    frame.draw_view_proj = frame.cull_view_proj;
    frame.time_s = 1.0F;
    return frame;
}

std::uint32_t tint_weight(const r::GrassBladeGpu& b) { return b.tint >> 24U; }
glm::ivec3 tint_rgb(const r::GrassBladeGpu& b) {
    return {static_cast<int>(b.tint & 0xFFU), static_cast<int>((b.tint >> 8U) & 0xFFU),
            static_cast<int>((b.tint >> 16U) & 0xFFU)};
}

} // namespace

// Clumped roots (on by default) are the CPU reference's, blade by blade and in
// total, and every root lies the configured share of the way to its clump centre.
TEST(GpuGrassV2, ClumpedPlacementMatchesTheCpuReference) {
    QUAT_REQUIRE_GPU(g);
    auto pass = r::GrassPass::create(g->device, g->allocator, g->cache, g->transfer, g->shader_dir, 1);
    ASSERT_OK(pass);
    r::GrassFrame frame;
    frame.settings = flat_settings();
    frame.settings.clump_size = 0.7F;
    frame.settings.clump_strength = 0.5F;
    frame.camera = glm::vec3(2.1F, 0.0F, -1.3F);
    frame.draw_view_proj = frame.cull_view_proj = overview(frame.camera);
    rhi::RenderGraph graph;
    auto gen = quat::test::run_grass(*g, *pass, graph, frame);
    ASSERT_OK(gen);

    const std::uint32_t reference = reference_placed(frame.settings, frame.camera, 0.0F);
    EXPECT_NEAR(static_cast<double>(gen->args.placed), reference, reference * 0.002 + 2.0);

    const r::GrassSettings& s = frame.settings;
    const float cell = r::grass_cell_size(s);
    const int reach = static_cast<int>(std::ceil(r::grass_max_clump_pull(s) / cell)) + 1;
    std::size_t checked = 0;
    for (std::size_t i = 0; i < gen->blades.size(); i += 211) {
        const r::GrassBladeGpu& bl = gen->blades[i];
        const glm::ivec2 home(static_cast<int>(std::floor(bl.root.x / cell)),
                              static_cast<int>(std::floor(bl.root.z / cell)));
        bool found = false;
        for (int dz = -reach; dz <= reach && !found; ++dz) {
            for (int dx = -reach; dx <= reach && !found; ++dx) {
                const glm::ivec2 c = home + glm::ivec2(dx, dz);
                const r::GrassCandidate ref = r::grass_candidate(s, cell, c);
                if (std::abs(ref.root_xz.x - bl.root.x) >= 1e-4F ||
                    std::abs(ref.root_xz.y - bl.root.z) >= 1e-4F ||
                    ref.hash != (bl.packed & 0xFFFFU)) {
                    continue;
                }
                found = true;
                EXPECT_EQ(ref.clump_hash, bl.clump);
                // The unpulled root and its clump: the blade went halfway.
                r::GrassSettings loose = s;
                loose.clump_size = 0.0F;
                const glm::vec2 own = r::grass_candidate(loose, cell, c).root_xz;
                const r::GrassClump clump = r::grass_clump(s, own);
                EXPECT_EQ(clump.hash, bl.clump);
                const glm::vec2 root(bl.root.x, bl.root.z);
                EXPECT_NEAR(glm::length(clump.centre - root),
                            glm::length(clump.centre - own) * (1.0F - s.clump_strength), 1e-4F);
                EXPECT_LE(glm::length(root - own), r::grass_max_clump_pull(s) + 1e-4F);
            }
        }
        EXPECT_TRUE(found) << "no reference candidate for the blade at " << bl.root.x << ", "
                           << bl.root.z;
        ++checked;
    }
    EXPECT_GT(checked, 50U);
}

// Seen through a perspective camera, blades narrower than min_blade_px thin
// out (as the CPU reference counts) and the survivors widen.
TEST(GpuGrassV2, FootprintLodThinsSubPixelBladesAndWidensTheRest) {
    QUAT_REQUIRE_GPU(g);
    auto pass = r::GrassPass::create(g->device, g->allocator, g->cache, g->transfer, g->shader_dir, 1);
    ASSERT_OK(pass);
    r::GrassFrame frame = ground_level_frame(glm::vec3(10.0F, 0.0F, 0.0F));
    frame.settings.min_blade_px = 2.0F;
    // Cull with an overview so both runs see every blade of the disc.
    frame.cull_view_proj = overview(frame.camera);
    const glm::mat4 projection = vulkan_perspective(glm::radians(60.0F), 1.0F, 0.1F, 300.0F);
    const float ppm = r::grass_pixels_per_metre(projection, k_extent.height);
    EXPECT_NEAR(ppm, 0.5F * 128.0F / std::tan(glm::radians(30.0F)), 1e-2F);
    EXPECT_EQ(r::grass_pixels_per_metre(glm::ortho(-1.0F, 1.0F, -1.0F, 1.0F), 128), 0.0F);

    rhi::RenderGraph graph_off;
    auto full = quat::test::run_grass(*g, *pass, graph_off, frame);
    ASSERT_OK(full);
    frame.pixels_per_metre = ppm;
    rhi::RenderGraph graph_on;
    auto thinned = quat::test::run_grass(*g, *pass, graph_on, frame);
    ASSERT_OK(thinned);

    const std::uint32_t reference = reference_placed(frame.settings, frame.camera, ppm);
    EXPECT_NEAR(static_cast<double>(thinned->args.placed), reference, reference * 0.003 + 3.0);
    EXPECT_LT(thinned->args.placed, full->args.placed * 3 / 4);

    // Width scale (bits 24-31) grows with distance; at 1 m it is 1.
    std::uint32_t near_scale = 0;
    std::uint32_t far_scale = 0;
    for (const r::GrassBladeGpu& b : thinned->blades) {
        const float d = glm::length(b.root - frame.camera);
        if (d < 1.5F) near_scale = std::max(near_scale, b.packed >> 24U);
        if (d > 12.0F) far_scale = std::max(far_scale, b.packed >> 24U);
    }
    EXPECT_EQ(near_scale, 0U);
    EXPECT_GT(far_scale, 100U);
}

// Blade roots record the ground albedo under them from the GBuffer; roots
// hidden behind the wall (by more than the pixel footprint along the ray)
// or off screen record no tint.
TEST(GpuGrassV2, RootsTakeTheGroundAlbedoWhereTheGroundIsVisible) {
    QUAT_REQUIRE_GPU(g);
    auto mesh = r::MeshPass::create(g->device, g->allocator, g->cache, g->transfer, g->shader_dir);
    ASSERT_OK(mesh);
    auto pass = r::GrassPass::create(g->device, g->allocator, g->cache, g->transfer, g->shader_dir, 1);
    ASSERT_OK(pass);
    const glm::vec3 ground_albedo(0.5F, 0.1F, 0.25F);
    auto scene = make_scene(*g, ground_albedo, glm::vec3(0.1F, 0.1F, 0.6F));
    ASSERT_OK(scene);

    r::GrassFrame frame = ground_level_frame(glm::vec3(10.0F, 0.0F, 0.0F));
    frame.settings.ground_cover = 0.0F; // the albedo under the roots stays the ground's own
    frame.settings.far_ground_cover = 0.0F;
    // The frame loop's projection scale: the tint tolerance follows the
    // pixel footprint along the ray.
    frame.pixels_per_metre = r::grass_pixels_per_metre(
        vulkan_perspective(glm::radians(60.0F), 1.0F, 0.1F, 300.0F), k_extent.height);
    rhi::TransientImagePool pool(g->device, g->allocator);
    rhi::RenderGraph graph(pool);
    const std::array<r::DrawItem, 2> items{
        r::DrawItem{&scene->ground, &scene->ground_material, glm::mat4(1.0F)},
        r::DrawItem{&scene->wall, &scene->wall_material, glm::mat4(1.0F)}};
    auto gbuffer = mesh->add_to_graph(graph, k_extent, frame.draw_view_proj, items);
    ASSERT_OK(gbuffer);
    auto gen = quat::test::run_grass(*g, *pass, graph, frame, &*gbuffer, k_extent);
    ASSERT_OK(gen);
    ASSERT_GT(gen->blades.size(), 500U);

    const glm::ivec3 expected(glm::round(glm::sqrt(ground_albedo) * 255.0F));
    std::size_t open = 0;
    std::size_t tinted = 0;
    std::size_t hidden = 0;
    for (const r::GrassBladeGpu& b : gen->blades) {
        const glm::vec3 to = b.root - frame.camera;
        // Off screen: no tint.
        const glm::vec4 clip = frame.draw_view_proj * glm::vec4(b.root, 1.0F);
        const glm::vec2 ndc = clip.w > 0.0F ? glm::vec2(clip) / clip.w : glm::vec2(9.0F);
        const float edge = std::max(std::abs(ndc.x), std::abs(ndc.y));
        if (edge > 0.97F) {
            if (edge > 1.01F) EXPECT_EQ(tint_weight(b), 0U);
            continue;
        }
        // Behind the wall: the ray to the root crosses x = 5 well inside it,
        // and the root lies more than two ground-pixel footprints behind it
        // (closer, the wall and the ground under the root share a pixel).
        const float t = 5.0F / to.x;
        const float dist = glm::length(to);
        const float footprint = dist / frame.pixels_per_metre / (frame.camera.y / dist);
        if (b.root.x > 5.0F + 2.0F * footprint && std::abs(to.z * t) < 0.9F) {
            ++hidden;
            EXPECT_EQ(tint_weight(b), 0U) << "hidden root at " << b.root.x << ", " << b.root.z;
            continue;
        }
        if (b.root.x < 4.7F) {
            ++open;
            if (tint_weight(b) == 255U) {
                ++tinted;
                const glm::ivec3 rgb = tint_rgb(b);
                EXPECT_NEAR(rgb.r, expected.r, 3);
                EXPECT_NEAR(rgb.g, expected.g, 3);
                EXPECT_NEAR(rgb.b, expected.b, 3);
            }
        }
    }
    EXPECT_GT(hidden, 20U);
    ASSERT_GT(open, 200U);
    EXPECT_GT(tinted, open * 95 / 100) << "visible roots without a tint";
}

// The ground cover tints the ground's albedo toward the blades' colour beyond
// the radius, leaves objects standing on it alone, and respects the growth
// limits (no cover where grass may not grow).
TEST(GpuGrassV2, GroundCoverTintsOnlyTheGroundGrassGrowsOn) {
    QUAT_REQUIRE_GPU(g);
    auto mesh = r::MeshPass::create(g->device, g->allocator, g->cache, g->transfer, g->shader_dir);
    ASSERT_OK(mesh);
    auto pass = r::GrassPass::create(g->device, g->allocator, g->cache, g->transfer, g->shader_dir, 1);
    ASSERT_OK(pass);
    const glm::vec3 grey(0.3F);
    const glm::vec3 blue(0.1F, 0.1F, 0.6F);
    auto scene = make_scene(*g, grey, blue);
    ASSERT_OK(scene);

    const auto render = [&](const r::GrassSettings& settings) {
        // Looking far down +X: the blades (radius 1.5 m) stay below the view.
        r::GrassFrame frame = ground_level_frame(glm::vec3(30.0F, 0.6F, 0.0F));
        frame.settings = settings;
        rhi::TransientImagePool pool(g->device, g->allocator);
        rhi::RenderGraph graph(pool);
        const std::array<r::DrawItem, 2> items{
            r::DrawItem{&scene->ground, &scene->ground_material, glm::mat4(1.0F)},
            r::DrawItem{&scene->wall, &scene->wall_material, glm::mat4(1.0F)}};
        auto gbuffer = mesh->add_to_graph(graph, k_extent, frame.draw_view_proj, items);
        EXPECT_TRUE(gbuffer.has_value());
        auto grass = pass->add_to_graph(graph, 0, frame, &*gbuffer, k_extent);
        EXPECT_TRUE(grass.has_value());
        return quat::test::render_to_host(*g, graph, gbuffer->albedo, k_extent, 4);
    };
    r::GrassSettings s = flat_settings();
    s.radius = 1.5F;
    s.far_ground_cover = 1.0F;
    auto covered = render(s);
    ASSERT_OK(covered);
    s.height_m = glm::vec2(1.0F, 2.0F); // the ground at y = 0 is out of range
    auto excluded = render(s);
    ASSERT_OK(excluded);

    // The wall's centre (x = 5, y = 1) and a ground pixel beside it.
    const glm::uvec2 wall = covered->centre() + glm::uvec2(0, 6);
    const glm::ivec4 w = covered->rgba8(wall.x, wall.y);
    EXPECT_NEAR(w.r, 26, 2);
    EXPECT_NEAR(w.g, 26, 2);
    EXPECT_NEAR(w.b, 153, 2);
    const glm::uvec2 ground(10, k_extent.height - 20);
    const glm::ivec4 c = covered->rgba8(ground.x, ground.y);
    EXPECT_GT(c.g, c.r) << "the covered ground did not turn toward the blades' green";
    EXPECT_GT(c.g, c.b);
    const glm::ivec4 bare = excluded->rgba8(ground.x, ground.y);
    EXPECT_NEAR(bare.r, 77, 2); // 0.3 untouched
    EXPECT_NEAR(bare.g, 77, 2);
    EXPECT_NEAR(bare.b, 77, 2);
    const glm::ivec4 w2 = excluded->rgba8(wall.x, wall.y);
    EXPECT_EQ(w2, w);
}

// #143 review: over terrain the ground cover only knows the ground inside the
// bound height tiles, so it must fade out before their edge rather than
// stop there (or, with terrain_only false, tint whatever lies at ground_y far
// beyond them). A flat 16 m tile around the camera: the ground 2.8 m away
// (inside the fade start) is tinted, the ground 26 m away (beyond the tile)
// keeps its albedo exactly.
TEST(GpuGrassV2, GroundCoverFadesOutBeforeTheBoundTilesEnd) {
    QUAT_REQUIRE_GPU(g);
    auto mesh = r::MeshPass::create(g->device, g->allocator, g->cache, g->transfer, g->shader_dir);
    ASSERT_OK(mesh);
    auto pass = r::GrassPass::create(g->device, g->allocator, g->cache, g->transfer, g->shader_dir, 1);
    ASSERT_OK(pass);
    auto scene = make_scene(*g, glm::vec3(0.3F), glm::vec3(0.1F, 0.1F, 0.6F));
    ASSERT_OK(scene);

    constexpr std::uint32_t res = 17;
    const std::vector<float> heights(res * res, 0.0F);
    auto image = rhi::upload_device_image(g->allocator, g->transfer, heights.data(),
                                          heights.size() * sizeof(float), {res, res},
                                          VK_FORMAT_R32_SFLOAT);
    ASSERT_OK(image);
    auto view = rhi::create_image_view(g->device.handle(), image->handle(), VK_FORMAT_R32_SFLOAT);
    ASSERT_OK(view);
    r::TerrainHeightTile tile;
    tile.view = view->handle();
    tile.origin_xz = glm::vec2(-8.0F);
    tile.size_m = 16.0F;
    tile.resolution = res;
    const std::array<r::TerrainHeightTile, 1> tiles{tile};

    r::GrassFrame frame = ground_level_frame(glm::vec3(30.0F, 0.6F, 0.0F));
    frame.settings = flat_settings(); // terrain_only false: flat fallback at y = 0 outside
    frame.settings.radius = 6.0F;     // fade 5.33 m (2/3 of the tile's 8 m reach) to 8 m
    frame.settings.height_range = glm::vec2(0.01F); // blades too small to hide the ground
    frame.settings.width_range = glm::vec2(0.001F);
    frame.settings.far_ground_cover = 1.0F;
    frame.terrain = tiles;
    rhi::TransientImagePool pool(g->device, g->allocator);
    rhi::RenderGraph graph(pool);
    const std::array<r::DrawItem, 2> items{
        r::DrawItem{&scene->ground, &scene->ground_material, glm::mat4(1.0F)},
        r::DrawItem{&scene->wall, &scene->wall_material, glm::mat4(1.0F)}};
    auto gbuffer = mesh->add_to_graph(graph, k_extent, frame.draw_view_proj, items);
    ASSERT_OK(gbuffer);
    ASSERT_OK(pass->add_to_graph(graph, 0, frame, &*gbuffer, k_extent));
    auto albedo = quat::test::render_to_host(*g, graph, gbuffer->albedo, k_extent, 4);
    ASSERT_OK(albedo);

    // Column 10 misses the wall. Row 108 sees the ground ~2.8 m away; row 67
    // sees it ~26 m away, far outside the tile.
    const glm::ivec4 near = albedo->rgba8(10, 108);
    EXPECT_LT(near.r, 72) << "the ground inside the tile was not covered";
    const glm::ivec4 far = albedo->rgba8(10, 67);
    EXPECT_NEAR(far.r, 77, 1) << "the cover reached past the bound tiles";
    EXPECT_NEAR(far.g, 77, 1);
    EXPECT_NEAR(far.b, 77, 1);
}
