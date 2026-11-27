// The tonemap pass at the end of the deferred chain (GBuffer -> lighting ->
// tonemap into RGBA8): a lit pixel is ACES-mapped and sRGB-encoded exactly as
// the CPU reference, and black HDR stays black.

#include <array>
#include <cmath>
#include <cstdint>
#include <expected>

#include <glm/glm.hpp>
#include <gtest/gtest.h>
#include <vulkan/vulkan.h>

#include "engine/asset/material_asset.hpp"
#include "engine/asset/mesh_asset.hpp"
#include "engine/renderer/lighting_pass.hpp"
#include "engine/renderer/mesh_pass.hpp"
#include "engine/renderer/tonemap_pass.hpp"
#include "engine/rhi/render_graph.hpp"
#include "render/pbr_reference.hpp"
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
constexpr VkFormat   k_ldr_format = VK_FORMAT_R8G8B8A8_UNORM;
constexpr glm::vec3  k_base{0.25F, 0.5F, 0.75F};

// A rough dielectric triangle lit head-on (N = V = L = +Z, no ambient),
// tonemapped into an RGBA8 target; rendered once for the suite.
std::expected<HostImage, Error> render_ldr(quat::test::GpuContext& g) {
    std::array<asset::Vertex, 3> verts{};
    verts[0].position = {-0.5F, -0.5F, 0.0F};
    verts[1].position = {0.5F, -0.5F, 0.0F};
    verts[2].position = {0.0F, 0.5F, 0.0F};
    const std::array<std::uint32_t, 3> indices{0, 1, 2};
    auto mesh = quat::test::upload_mesh(g, verts, indices);
    if (!mesh) return std::unexpected(mesh.error());
    asset::PbrMaterialParams params;
    params.base_color_factor = {k_base, 1.0F};
    params.metallic_factor = 0.0F;
    params.roughness_factor = 1.0F;
    auto material = asset::upload_material(params, asset::MaterialTextures{}, g.allocator,
                                           g.transfer);
    if (!material) return std::unexpected(material.error());

    auto mesh_pass = r::MeshPass::create(g.device, g.allocator, g.cache, g.transfer, g.shader_dir);
    if (!mesh_pass) return std::unexpected(mesh_pass.error());
    auto lighting = r::LightingPass::create(g.device, g.allocator, g.cache, g.shader_dir);
    if (!lighting) return std::unexpected(lighting.error());
    auto tonemap =
        r::TonemapPass::create(g.device, g.allocator, g.cache, g.shader_dir, k_ldr_format);
    if (!tonemap) return std::unexpected(tonemap.error());

    r::DirectionalLightParams light;
    light.direction = {0.0F, 0.0F, 1.0F, 0.0F};
    light.color = {1.0F, 1.0F, 1.0F, 1.0F};
    light.ambient = {0.0F, 0.0F, 0.0F, 0.0F};

    rhi::TransientImagePool pool(g.device, g.allocator);
    rhi::RenderGraph graph(pool);
    const r::DrawItem item{&*mesh, &*material, glm::mat4(1.0F)};
    auto targets = mesh_pass->add_to_graph(graph, k_extent, glm::mat4(1.0F), {&item, 1});
    if (!targets) return std::unexpected(targets.error());
    auto hdr = lighting->add_to_graph(graph, *targets, k_extent, light, glm::mat4(1.0F),
                                      glm::vec3(0.0F, 0.0F, 1.0F), rhi::ResourceHandle{},
                                      glm::mat4(1.0F));
    if (!hdr) return std::unexpected(hdr.error());
    const rhi::ResourceHandle ldr = graph.create_transient_image(
        "ldr_color", k_ldr_format, k_extent,
        VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT);
    if (auto added = tonemap->add_to_graph(graph, *hdr, ldr, k_extent); !added) {
        return std::unexpected(added.error());
    }
    return quat::test::render_to_host(g, graph, ldr, k_extent, 4);
}

const std::expected<HostImage, Error>& ldr_frame(quat::test::GpuContext& g) {
    static const std::expected<HostImage, Error> image = render_ldr(g);
    return image;
}

} // namespace

// Expected LDR = sRGB(ACES(lit)). The GBuffer quantised the base colour to 8
// bits; light that value head-on through the shared PBR reference, then apply
// the shader's tonemap.
TEST(GpuTonemap, LitPixelIsAcesThenSrgb) {
    QUAT_REQUIRE_GPU(g);
    const auto& ldr = ldr_frame(*g);
    ASSERT_OK(ldr);
    glm::vec3 quantised;
    for (int i = 0; i < 3; ++i) quantised[i] = std::round(k_base[i] * 255.0F) / 255.0F;
    const glm::vec3 z(0.0F, 0.0F, 1.0F);
    const glm::vec3 lit =
        quat::test::pbr_ref::direct(quantised, 0.0F, 1.0F, z, z, z, glm::vec3(1.0F), 1.0F);
    const glm::vec3 mapped = quat::test::pbr_ref::aces(lit);

    const glm::uvec2 c = ldr->centre();
    const glm::ivec4 px = ldr->rgba8(c.x, c.y);
    for (int k = 0; k < 3; ++k) {
        const int expected =
            static_cast<int>(std::round(quat::test::pbr_ref::linear_to_srgb(mapped[k]) * 255.0F));
        EXPECT_NEAR(px[k], expected, 2) << "channel " << k;
    }
}

// Background HDR is 0 -> ACES(0) = 0 -> sRGB(0) = 0.
TEST(GpuTonemap, BlackHdrStaysBlack) {
    QUAT_REQUIRE_GPU(g);
    const auto& ldr = ldr_frame(*g);
    ASSERT_OK(ldr);
    EXPECT_EQ(glm::ivec3(ldr->rgba8(1, 1)), glm::ivec3(0));
}
