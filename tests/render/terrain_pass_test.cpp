// The terrain geometry pass: a generated tile uploaded with set_tile and drawn
// over a cleared GBuffer from a top-down camera lands terrain-coloured texels
// in the albedo target (the geometry pipeline, vertex texture fetch and splat
// shading all ran).

#include <expected>

#include <glm/glm.hpp>
#include <glm/gtc/matrix_transform.hpp>
#include <gtest/gtest.h>
#include <vulkan/vulkan.h>

#include "engine/renderer/mesh_pass.hpp"
#include "engine/renderer/terrain_pass.hpp"
#include "engine/rhi/render_graph.hpp"
#include "engine/terrain/generator.hpp"
#include "support/expect_ok.hpp"
#include "support/gpu_context.hpp"
#include "support/render_readback.hpp"

namespace {

namespace r = engine::renderer;
namespace rhi = engine::rhi;
using engine::core::Error;
using quat::test::HostImage;

std::expected<HostImage, Error> render_tile(quat::test::GpuContext& g) {
    auto mesh_pass = r::MeshPass::create(g.device, g.allocator, g.cache, g.transfer, g.shader_dir);
    if (!mesh_pass) return std::unexpected(mesh_pass.error());
    auto pass = r::TerrainPass::create(g.device, g.allocator, g.cache, g.transfer, g.shader_dir);
    if (!pass) return std::unexpected(pass.error());

    engine::terrain::TerrainParams tp;
    tp.resolution = 129;
    tp.tile_size_m = 256.0F;
    tp.height_m = 40.0F;
    tp.octaves = 5;
    tp.erosion_droplets = 2'000;
    const engine::terrain::Heightmap map = engine::terrain::generate(tp);
    if (auto set = pass->set_tile(glm::ivec2(0, 0), map, g.transfer); !set) {
        return std::unexpected(set.error());
    }

    // Top-down camera over the tile centre.
    constexpr VkExtent2D extent{128, 128};
    const glm::vec3 eye(128.0F, 300.0F, 128.0F);
    const glm::mat4 view =
        glm::lookAt(eye, glm::vec3(128.0F, 0.0F, 128.0F), glm::vec3(0.0F, 0.0F, 1.0F));
    glm::mat4 proj = glm::perspective(glm::radians(60.0F), 1.0F, 0.1F, 1000.0F);
    proj[1][1] *= -1.0F; // Vulkan clip-space Y flip, matching the runtime camera
    const glm::mat4 vp = proj * view;

    rhi::TransientImagePool pool(g.device, g.allocator);
    rhi::RenderGraph graph(pool);
    auto targets = mesh_pass->add_to_graph(graph, extent, vp, r::InstancedDrawList{}); // clears
    if (!targets) return std::unexpected(targets.error());
    pass->add_to_graph(graph, *targets, extent, vp, eye, glm::vec3(128.0F, 0.0F, 128.0F),
                       /*snowline_m=*/1000.0F);
    return quat::test::render_to_host(g, graph, targets->albedo, extent, 4);
}

const std::expected<HostImage, Error>& tile_albedo(quat::test::GpuContext& g) {
    static const std::expected<HostImage, Error> image = render_tile(g);
    return image;
}

} // namespace

// Cleared albedo is (0,0,0,0); a rasterised terrain texel has alpha 1.
TEST(GpuTerrainPass, RasterisesTheTileUnderTheCamera) {
    QUAT_REQUIRE_GPU(g);
    const auto& albedo = tile_albedo(*g);
    ASSERT_OK(albedo);
    const glm::uvec2 c = albedo->centre();
    EXPECT_GE(albedo->rgba8(c.x, c.y).a, 200) << "no terrain rasterised at the centre";
}

// A green-leaning ground colour (snow is disabled by the high snowline).
TEST(GpuTerrainPass, CentreTexelIsTerrainColoured) {
    QUAT_REQUIRE_GPU(g);
    const auto& albedo = tile_albedo(*g);
    ASSERT_OK(albedo);
    const glm::uvec2 c = albedo->centre();
    const glm::ivec4 px = albedo->rgba8(c.x, c.y);
    EXPECT_NE(px.g, 0);
    EXPECT_GE(px.g + px.r, 10);
}
