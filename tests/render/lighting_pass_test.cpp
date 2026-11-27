// The deferred lighting pass on a GBuffer triangle, read back from the HDR
// target: the direct Cook-Torrance BRDF against the CPU reference, the
// clearcoat lobe, and image-based lighting from baked IBL maps.

#include <array>
#include <cmath>
#include <cstdint>
#include <expected>
#include <utility>

#include <glm/glm.hpp>
#include <gtest/gtest.h>
#include <vulkan/vulkan.h>

#include "engine/asset/material_asset.hpp"
#include "engine/asset/mesh_asset.hpp"
#include "engine/renderer/ibl_pass.hpp"
#include "engine/renderer/lighting_pass.hpp"
#include "engine/renderer/mesh_pass.hpp"
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

struct Scene {
    asset::PbrMaterialParams  material;
    r::DirectionalLightParams light;
    float                     half_size = 0.5F; // clip-space triangle extent
    bool                      sky = false;
    r::IblViewSet             ibl{};
    r::LightingTuning         tuning{}; // A8 render settings (IBL scale, shadow filter)
};

// Head-on directional light along +Z (the triangle's normal), white,
// intensity 1, no ambient.
r::DirectionalLightParams head_on_light() {
    r::DirectionalLightParams light;
    light.direction = {0.0F, 0.0F, 1.0F, 0.0F};
    light.color = {1.0F, 1.0F, 1.0F, 1.0F};
    light.ambient = {0.0F, 0.0F, 0.0F, 0.0F};
    return light;
}

// GBuffer -> lighting for a camera-facing clip-space triangle (identity
// view_proj, camera at +Z so N = V = L at the centre); returns the HDR target.
std::expected<HostImage, Error> render_lit(quat::test::GpuContext& g, const Scene& scene) {
    const float s = scene.half_size;
    std::array<asset::Vertex, 3> verts{};
    verts[0].position = {-s, -s, 0.0F};
    verts[1].position = {s, -s, 0.0F};
    verts[2].position = {0.0F, s, 0.0F};
    const std::array<std::uint32_t, 3> indices{0, 1, 2};
    auto mesh = quat::test::upload_mesh(g, verts, indices);
    if (!mesh) return std::unexpected(mesh.error());
    auto material = asset::upload_material(scene.material, asset::MaterialTextures{}, g.allocator,
                                           g.transfer);
    if (!material) return std::unexpected(material.error());
    auto mesh_pass = r::MeshPass::create(g.device, g.allocator, g.cache, g.transfer, g.shader_dir);
    if (!mesh_pass) return std::unexpected(mesh_pass.error());
    auto lighting = r::LightingPass::create(g.device, g.allocator, g.cache, g.shader_dir);
    if (!lighting) return std::unexpected(lighting.error());

    rhi::TransientImagePool pool(g.device, g.allocator);
    rhi::RenderGraph graph(pool);
    const r::DrawItem item{&*mesh, &*material, glm::mat4(1.0F)};
    auto targets = mesh_pass->add_to_graph(graph, k_extent, glm::mat4(1.0F), {&item, 1});
    if (!targets) return std::unexpected(targets.error());
    auto hdr = lighting->add_to_graph(graph, *targets, k_extent, scene.light, glm::mat4(1.0F),
                                      glm::vec3(0.0F, 0.0F, 1.0F), rhi::ResourceHandle{},
                                      glm::mat4(1.0F), {}, scene.sky, scene.ibl, VK_NULL_HANDLE,
                                      VK_NULL_HANDLE, {}, scene.tuning);
    if (!hdr) return std::unexpected(hdr.error());
    return quat::test::render_to_host(g, graph, *hdr, k_extent, 8);
}

[[nodiscard]] glm::vec3 centre_rgb(const HostImage& hdr) {
    const glm::uvec2 c = hdr.centre();
    return glm::vec3(hdr.rgba16f(c.x, c.y));
}

[[nodiscard]] float quant8(float v) { return std::round(v * 255.0F) / 255.0F; }

// A smooth metal: diffuse is killed (metallic = 1), so the lit value is pure
// Cook-Torrance specular — a strong head-on highlight a Lambert shader could
// never produce, sensitive to the whole BRDF (D/Vis/F). Values land exactly on
// 8-bit UNORM steps so GPU and std::round agree (0.3 would hit the 76.5
// half-step where the two round differently).
constexpr glm::vec3 k_metal_base{0.25F, 0.5F, 0.75F};
constexpr float     k_metal_metallic = 1.0F;  // 255
constexpr float     k_metal_roughness = 0.5F; // 127.5 -> 128 either way

const std::expected<HostImage, Error>& lit_metal(quat::test::GpuContext& g) {
    static const std::expected<HostImage, Error> image = [&] {
        Scene scene;
        scene.material.base_color_factor = {k_metal_base, 1.0F};
        scene.material.metallic_factor = k_metal_metallic;
        scene.material.roughness_factor = k_metal_roughness;
        scene.light = head_on_light();
        return render_lit(g, scene);
    }();
    return image;
}

// A fully rough dielectric base (pure diffuse, so any sharp head-on highlight
// can only come from the coat lobe), without and with a smooth clearcoat.
const std::expected<std::pair<glm::vec3, glm::vec3>, Error>&
uncoated_and_coated(quat::test::GpuContext& g) {
    static const std::expected<std::pair<glm::vec3, glm::vec3>, Error> centres =
        [&]() -> std::expected<std::pair<glm::vec3, glm::vec3>, Error> {
        Scene scene;
        scene.material.base_color_factor = {0.25F, 0.5F, 0.75F, 1.0F};
        scene.material.metallic_factor = 0.0F;
        scene.material.roughness_factor = 1.0F;
        scene.material.clearcoat_roughness = 0.2F;
        scene.light = head_on_light();
        scene.material.clearcoat_factor = 0.0F;
        auto base = render_lit(g, scene);
        if (!base) return std::unexpected(base.error());
        scene.material.clearcoat_factor = 1.0F;
        auto coated = render_lit(g, scene);
        if (!coated) return std::unexpected(coated.error());
        return std::pair{centre_rgb(*base), centre_rgb(*coated)};
    }();
    return centres;
}

// A smooth metal (no diffuse: any lit value is the environment reflection)
// under no direct light and no ambient — only image-based lighting.
Scene environment_only_metal() {
    Scene scene;
    scene.material.base_color_factor = {0.6F, 0.6F, 0.6F, 1.0F};
    scene.material.metallic_factor = 1.0F;
    scene.material.roughness_factor = 0.05F;
    scene.light.color = {1.0F, 1.0F, 1.0F, 0.0F}; // intensity 0
    scene.light.ambient = {0.0F, 0.0F, 0.0F, 0.0F};
    scene.half_size = 0.9F;
    return scene;
}

} // namespace

// The GBuffer quantised albedo/metallic/roughness to 8 bits. The centre
// pixel's view vector is reconstructed exactly as the shader does (identity
// view_proj, surface depth 0, camera at +Z): low-roughness specular is too
// sharp to assume perfect head-on for a pixel off the projection centre.
TEST(GpuLighting, LitPixelMatchesThePbrReference) {
    QUAT_REQUIRE_GPU(g);
    const auto& hdr = lit_metal(*g);
    ASSERT_OK(hdr);
    const glm::uvec2 c = hdr->centre();
    const float u = (static_cast<float>(c.x) + 0.5F) / static_cast<float>(k_extent.width);
    const float v = (static_cast<float>(c.y) + 0.5F) / static_cast<float>(k_extent.height);
    const glm::vec3 surface(u * 2.0F - 1.0F, v * 2.0F - 1.0F, 0.0F);
    const glm::vec3 n(0.0F, 0.0F, 1.0F);
    const glm::vec3 view = glm::normalize(glm::vec3(0.0F, 0.0F, 1.0F) - surface);
    const glm::vec3 base_q{quant8(k_metal_base.x), quant8(k_metal_base.y),
                           quant8(k_metal_base.z)};
    const glm::vec3 expected =
        quat::test::pbr_ref::direct(base_q, quant8(k_metal_metallic), quant8(k_metal_roughness),
                                    n, view, n, glm::vec3(1.0F), 1.0F);
    const glm::vec3 lit = centre_rgb(*hdr);
    for (int k = 0; k < 3; ++k) EXPECT_NEAR(lit[k], expected[k], 0.02F) << "channel " << k;
}

// The metallic highlight is strong (brighter than the albedo), not a dim
// diffuse term — a guard against a regression that drops the specular lobe.
TEST(GpuLighting, MetallicHighlightOutshinesTheAlbedo) {
    QUAT_REQUIRE_GPU(g);
    const auto& hdr = lit_metal(*g);
    ASSERT_OK(hdr);
    EXPECT_GE(centre_rgb(*hdr).b, quant8(k_metal_base.z));
}

TEST(GpuLighting, BackgroundStaysBlack) {
    QUAT_REQUIRE_GPU(g);
    const auto& hdr = lit_metal(*g);
    ASSERT_OK(hdr);
    EXPECT_EQ(glm::vec3(hdr->rgba16f(1, 1)), glm::vec3(0.0F));
}

// The smooth coat adds a strong head-on highlight the rough diffuse base
// cannot produce (D_GGX(a=0.04) * Vis * F0c is far above the diffuse).
TEST(GpuClearcoat, CoatAddsASpecularHighlight) {
    QUAT_REQUIRE_GPU(g);
    const auto& centres = uncoated_and_coated(*g);
    ASSERT_OK(centres);
    const auto& [base, coated] = *centres;
    EXPECT_GE(coated.r - base.r, 0.5F);
}

// The coat is achromatic: the highlight it adds is (near) equal across
// channels (the 4% base attenuation difference is far below this tolerance).
TEST(GpuClearcoat, CoatHighlightIsAchromatic) {
    QUAT_REQUIRE_GPU(g);
    const auto& centres = uncoated_and_coated(*g);
    ASSERT_OK(centres);
    const auto& [base, coated] = *centres;
    const float dr = coated.r - base.r;
    const float db = coated.b - base.b;
    EXPECT_LE(std::abs(dr - db), 0.1F + 0.05F * dr);
}

// With IBL the metal reflects the sky: non-black and blue-dominant.
TEST(GpuIblLighting, MetalReflectsTheEnvironment) {
    QUAT_REQUIRE_GPU(g);
    auto baker = r::IblBaker::create(g->device, g->allocator, g->cache, g->shader_dir);
    ASSERT_OK(baker);
    auto maps = baker->bake(glm::normalize(glm::vec3(0.3F, 1.0F, 0.3F)));
    ASSERT_OK(maps);
    Scene scene = environment_only_metal();
    scene.sky = true;
    scene.ibl = maps->views();
    auto hdr = render_lit(*g, scene);
    ASSERT_OK(hdr);
    const glm::vec3 lit = centre_rgb(*hdr);
    EXPECT_GT(lit.r + lit.g + lit.b, 0.02F);
    EXPECT_GE(lit.b, lit.r) << "the reflection is not sky-tinted";
}

// A8: ibl.intensity scales the image-based term linearly (0 removes it).
TEST(GpuIblLighting, IntensityScalesTheReflection) {
    QUAT_REQUIRE_GPU(g);
    auto baker = r::IblBaker::create(g->device, g->allocator, g->cache, g->shader_dir);
    ASSERT_OK(baker);
    auto maps = baker->bake(glm::normalize(glm::vec3(0.3F, 1.0F, 0.3F)));
    ASSERT_OK(maps);
    Scene scene = environment_only_metal();
    scene.sky = true;
    scene.ibl = maps->views();
    auto base = render_lit(*g, scene);
    ASSERT_OK(base);
    scene.tuning.ibl_intensity = 2.0F;
    auto doubled = render_lit(*g, scene);
    ASSERT_OK(doubled);
    scene.tuning.ibl_intensity = 0.0F;
    auto none = render_lit(*g, scene);
    ASSERT_OK(none);
    const glm::vec3 b = centre_rgb(*base);
    const glm::vec3 d = centre_rgb(*doubled);
    ASSERT_GT(b.b, 0.01F);
    for (int k = 0; k < 3; ++k) EXPECT_NEAR(d[k], 2.0F * b[k], 0.01F * d[k] + 1e-3F) << k;
    EXPECT_LE(glm::length(centre_rgb(*none)), 1e-4F);
}

// With the environment disabled (and no direct light) the metal is black.
TEST(GpuIblLighting, NoEnvironmentAndNoLightIsBlack) {
    QUAT_REQUIRE_GPU(g);
    auto hdr = render_lit(*g, environment_only_metal());
    ASSERT_OK(hdr);
    const glm::vec3 lit = centre_rgb(*hdr);
    EXPECT_LE(lit.r + lit.g + lit.b, 0.02F);
}
