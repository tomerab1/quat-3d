// The GBuffer (mesh) pass: a clip-space triangle writes its material's base
// colour into the albedo target and leaves uncovered pixels at the clear
// value; geometric specular anti-aliasing widens the stored roughness only
// where the vertex normals vary across the pixel.

#include <array>
#include <cstdint>
#include <cstdlib>
#include <expected>

#include <glm/glm.hpp>
#include <gtest/gtest.h>
#include <vulkan/vulkan.h>

#include "engine/asset/material_asset.hpp"
#include "engine/asset/mesh_asset.hpp"
#include "engine/renderer/mesh_pass.hpp"
#include "engine/rhi/render_graph.hpp"
#include "support/expect_ok.hpp"
#include "support/gpu_context.hpp"
#include "support/render_readback.hpp"

namespace {

namespace r = engine::renderer;
namespace rhi = engine::rhi;
namespace asset = engine::asset;
using engine::core::Error;
using quat::test::HostImage;

constexpr VkExtent2D k_extent{64, 64};

enum class Target { albedo, material };

// A camera-facing clip-space triangle (identity view_proj + model) with the
// given vertex normals, drawn with `params`; returns the chosen RGBA8 target.
std::expected<HostImage, Error> render_triangle(quat::test::GpuContext& g,
                                                const asset::PbrMaterialParams& params,
                                                const std::array<glm::vec3, 3>& normals,
                                                Target target) {
    std::array<asset::Vertex, 3> verts{};
    verts[0].position = {-0.5F, -0.5F, 0.0F};
    verts[1].position = {0.5F, -0.5F, 0.0F};
    verts[2].position = {0.0F, 0.5F, 0.0F};
    for (std::size_t i = 0; i < 3; ++i) verts[i].normal = normals[i];
    const std::array<std::uint32_t, 3> indices{0, 1, 2};
    auto mesh = quat::test::upload_mesh(g, verts, indices);
    if (!mesh) return std::unexpected(mesh.error());
    auto material = asset::upload_material(params, asset::MaterialTextures{}, g.allocator,
                                           g.transfer);
    if (!material) return std::unexpected(material.error());
    auto pass = r::MeshPass::create(g.device, g.allocator, g.cache, g.transfer, g.shader_dir);
    if (!pass) return std::unexpected(pass.error());

    rhi::TransientImagePool pool(g.device, g.allocator);
    rhi::RenderGraph graph(pool);
    const r::DrawItem item{&*mesh, &*material, glm::mat4(1.0F)};
    auto targets = pass->add_to_graph(graph, k_extent, glm::mat4(1.0F), {&item, 1});
    if (!targets) return std::unexpected(targets.error());
    return quat::test::render_to_host(
        g, graph, target == Target::albedo ? targets->albedo : targets->material, k_extent, 4);
}

constexpr glm::vec3 k_up(0.0F, 0.0F, 1.0F);

// Base colour (0.25, 0.5, 0.75), no textures; rendered once for the suite.
const std::expected<HostImage, Error>& base_colour_albedo(quat::test::GpuContext& g) {
    static const std::expected<HostImage, Error> image = [&] {
        asset::PbrMaterialParams params;
        params.base_color_factor = {0.25F, 0.5F, 0.75F, 1.0F};
        return render_triangle(g, params, {k_up, k_up, k_up}, Target::albedo);
    }();
    return image;
}

// Authored roughness 0: any roughness in the material target (.g) can only
// come from the specular-AA normal-variance widening.
std::expected<int, Error> centre_roughness(quat::test::GpuContext& g,
                                           const std::array<glm::vec3, 3>& normals) {
    asset::PbrMaterialParams params;
    params.roughness_factor = 0.0F;
    auto image = render_triangle(g, params, normals, Target::material);
    if (!image) return std::unexpected(image.error());
    const glm::uvec2 c = image->centre();
    return image->rgba8(c.x, c.y).g;
}

} // namespace

TEST(GpuGBuffer, CoveredPixelHoldsTheMaterialBaseColour) {
    QUAT_REQUIRE_GPU(g);
    const auto& albedo = base_colour_albedo(*g);
    ASSERT_OK(albedo);
    const glm::uvec2 c = albedo->centre();
    const glm::ivec4 px = albedo->rgba8(c.x, c.y);
    EXPECT_NEAR(px.r, 64, 2);
    EXPECT_NEAR(px.g, 128, 2);
    EXPECT_NEAR(px.b, 191, 2);
}

TEST(GpuGBuffer, UncoveredPixelKeepsTheClearColour) {
    QUAT_REQUIRE_GPU(g);
    const auto& albedo = base_colour_albedo(*g);
    ASSERT_OK(albedo);
    EXPECT_EQ(albedo->rgba8(1, 1), glm::ivec4(0));
}

// Constant normals have zero screen-space variance: roughness stays at the
// authored 0 (this also guards the pixel-exact lighting/tonemap tests against
// AA drift).
TEST(GpuSpecularAa, FlatSurfaceKeepsItsAuthoredRoughness) {
    QUAT_REQUIRE_GPU(g);
    auto roughness = centre_roughness(*g, {k_up, k_up, k_up});
    ASSERT_OK(roughness);
    EXPECT_LE(*roughness, 2);
}

// Wildly divergent vertex normals: large per-pixel normal derivatives widen
// the stored roughness even though the authored roughness is 0.
TEST(GpuSpecularAa, DivergentNormalsWidenTheRoughness) {
    QUAT_REQUIRE_GPU(g);
    auto roughness = centre_roughness(*g, {glm::vec3(1.0F, 0.0F, 0.0F),
                                           glm::vec3(-1.0F, 0.0F, 0.0F),
                                           glm::vec3(0.0F, 1.0F, 0.0F)});
    ASSERT_OK(roughness);
    EXPECT_GE(*roughness, 20);
}
