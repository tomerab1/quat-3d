// KTX2 textures on the GPU (R17): a cooked KTX2 is transcoded for the
// device's formats (BC on desktop / lavapipe), uploaded with its mip chain,
// and sampled by the GBuffer pass as a material's base colour. The view
// swizzles (BC4 grey, X/Y normal layout) are checked the same way.

#include <array>
#include <cmath>
#include <cstdint>
#include <expected>
#include <vector>

#include <glm/glm.hpp>
#include <gtest/gtest.h>
#include <vulkan/vulkan.h>

#include "engine/asset/asset_manager.hpp"
#include "engine/asset/ktx2_texture.hpp"
#include "engine/asset/material_asset.hpp"
#include "engine/cook/gltf_cooker.hpp"
#include "engine/renderer/mesh_pass.hpp"
#include "engine/rhi/render_graph.hpp"
#include "support/expect_ok.hpp"
#include "support/gpu_context.hpp"
#include "support/render_readback.hpp"

namespace {

namespace asset = engine::asset;
namespace r = engine::renderer;
namespace rhi = engine::rhi;
using engine::core::Error;

constexpr VkExtent2D k_extent{64, 64};
constexpr std::uint32_t k_tex = 32;

std::vector<std::byte> solid(std::array<std::uint8_t, 4> rgba) {
    std::vector<std::byte> px(k_tex * k_tex * 4);
    for (std::size_t i = 0; i < px.size(); ++i) px[i] = static_cast<std::byte>(rgba[i % 4]);
    return px;
}

// Encodes `rgba` like the cooker, decodes it for `caps`.
std::expected<asset::TextureData, Error> cooked(const std::vector<std::byte>& rgba,
                                                asset::TextureRole role,
                                                asset::TextureColorSpace space,
                                                rhi::TextureCompressionSupport caps) {
    engine::cook::TextureCookOptions o;
    o.color_codec = engine::cook::BasisCodec::uastc;
    o.threads = 1;
    auto file = engine::cook::encode_ktx2(rgba, k_tex, k_tex, role, space, o);
    if (!file) return std::unexpected(file.error());
    asset::Ktx2DecodeOptions d;
    d.caps = caps;
    d.role = role;
    d.color_space = space;
    return asset::decode_ktx2(*file, d);
}

// Draws a triangle whose material samples `texture` as base colour (constant
// UV) and returns the albedo at the centre pixel.
std::expected<glm::ivec4, Error> albedo_of(quat::test::GpuContext& g,
                                           const asset::TextureData& texture) {
    asset::AssetManager assets;
    auto handle = assets.load<asset::TextureAsset>("ktx2-test", [&](const auto&) {
        return asset::upload_texture_data(texture, g.allocator, g.transfer);
    });
    if (!handle.is_loaded()) return std::unexpected(Error{"texture upload failed"});

    std::array<asset::Vertex, 3> verts{};
    verts[0].position = {-0.5F, -0.5F, 0.0F};
    verts[1].position = {0.5F, -0.5F, 0.0F};
    verts[2].position = {0.0F, 0.5F, 0.0F};
    for (asset::Vertex& v : verts) v.uv = {0.5F, 0.5F};
    const std::array<std::uint32_t, 3> indices{0, 1, 2};
    auto mesh = quat::test::upload_mesh(g, verts, indices);
    if (!mesh) return std::unexpected(mesh.error());

    asset::PbrMaterialParams params;
    params.flags = asset::material_has_base_color;
    asset::MaterialTextures textures;
    textures.base_color = handle;
    auto material = asset::upload_material(params, textures, g.allocator, g.transfer);
    if (!material) return std::unexpected(material.error());
    auto pass = r::MeshPass::create(g.device, g.allocator, g.cache, g.transfer, g.shader_dir);
    if (!pass) return std::unexpected(pass.error());

    rhi::TransientImagePool pool(g.device, g.allocator);
    rhi::RenderGraph graph(pool);
    const r::DrawItem item{&*mesh, &*material, glm::mat4(1.0F)};
    auto targets = pass->add_to_graph(graph, k_extent, glm::mat4(1.0F), {&item, 1});
    if (!targets) return std::unexpected(targets.error());
    auto image = quat::test::render_to_host(g, graph, targets->albedo, k_extent, 4);
    if (!image) return std::unexpected(image.error());
    const glm::uvec2 c = image->centre();
    return image->rgba8(c.x, c.y);
}

int srgb_to_linear8(int c) {
    const float v = static_cast<float>(c) / 255.0F;
    const float l = v <= 0.04045F ? v / 12.92F : std::pow((v + 0.055F) / 1.055F, 2.4F);
    return static_cast<int>(std::lround(l * 255.0F));
}

} // namespace

TEST(GpuKtx2Texture, Bc7BaseColourIsSampledWithItsColour) {
    QUAT_REQUIRE_GPU(g);
    if (!g->transfer.compression.bc) GTEST_SKIP() << "device has no BC formats";
    auto tex = cooked(solid({200, 100, 50, 255}), asset::TextureRole::color,
                      asset::TextureColorSpace::srgb, g->transfer.compression);
    ASSERT_OK(tex);
    ASSERT_EQ(tex->format, asset::TexelFormat::bc7);
    EXPECT_EQ(tex->levels.size(), 6U); // 32 -> 1
    auto px = albedo_of(*g, *tex);
    ASSERT_OK(px);
    EXPECT_NEAR(px->r, srgb_to_linear8(200), 3);
    EXPECT_NEAR(px->g, srgb_to_linear8(100), 3);
    EXPECT_NEAR(px->b, srgb_to_linear8(50), 3);
}

TEST(GpuKtx2Texture, Bc4SingleChannelReadsAsGrey) {
    QUAT_REQUIRE_GPU(g);
    if (!g->transfer.compression.bc) GTEST_SKIP() << "device has no BC formats";
    auto tex = cooked(solid({180, 20, 20, 255}), asset::TextureRole::single_channel,
                      asset::TextureColorSpace::linear, g->transfer.compression);
    ASSERT_OK(tex);
    ASSERT_EQ(tex->format, asset::TexelFormat::bc4);
    auto px = albedo_of(*g, *tex);
    ASSERT_OK(px);
    EXPECT_NEAR(px->r, 180, 3);
    EXPECT_NEAR(px->g, 180, 3);
    EXPECT_NEAR(px->b, 180, 3);
}

// X/Y normal layout without BC5: RGBA8 data with the view moving alpha to G.
TEST(GpuKtx2Texture, XyNormalLayoutIsSwizzledIntoRedGreen) {
    QUAT_REQUIRE_GPU(g);
    const float nx = 0.3F;
    const float ny = -0.4F;
    const float nz = std::sqrt(1.0F - nx * nx - ny * ny);
    const auto unorm = [](float v) { return static_cast<std::uint8_t>(std::lround((v * 0.5F + 0.5F) * 255.0F)); };
    auto tex = cooked(solid({unorm(nx), unorm(ny), unorm(nz), 255}), asset::TextureRole::normal,
                      asset::TextureColorSpace::linear, rhi::TextureCompressionSupport{});
    ASSERT_OK(tex);
    ASSERT_EQ(tex->format, asset::TexelFormat::rgba8);
    ASSERT_EQ(tex->swizzle, asset::TextureSwizzle::ra01);
    auto px = albedo_of(*g, *tex);
    ASSERT_OK(px);
    EXPECT_NEAR(px->r, unorm(nx), 3);
    EXPECT_NEAR(px->g, unorm(ny), 3);
    EXPECT_NEAR(px->b, 0, 1);
}

TEST(GpuKtx2Texture, AstcUploadsWhereSupported) {
    QUAT_REQUIRE_GPU(g);
    if (!g->transfer.compression.astc_ldr) GTEST_SKIP() << "device has no ASTC LDR (lavapipe)";
    auto tex = cooked(solid({30, 160, 220, 255}), asset::TextureRole::color,
                      asset::TextureColorSpace::linear,
                      rhi::TextureCompressionSupport{false, true, true});
    ASSERT_OK(tex);
    ASSERT_EQ(tex->format, asset::TexelFormat::astc_4x4);
    auto px = albedo_of(*g, *tex);
    ASSERT_OK(px);
    EXPECT_NEAR(px->r, 30, 3);
    EXPECT_NEAR(px->g, 160, 3);
    EXPECT_NEAR(px->b, 220, 3);
}
