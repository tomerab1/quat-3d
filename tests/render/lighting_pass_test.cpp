// The deferred lighting pass on a GBuffer triangle, read back from the HDR
// target: the direct Cook-Torrance BRDF against the CPU reference, the
// clearcoat lobe, image-based lighting from baked IBL maps, and the A8 lighting
// tuning (IBL intensity; on the R12 cascades: shadow filter radius, texel-unit
// biases, shadows off).

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <cstring>
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
#include "engine/renderer/csm.hpp"
#include "engine/renderer/shadow_pass.hpp"
#include "engine/rhi/gpu_allocator.hpp"
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
    r::LightingTuning         tuning{}; // A8 render settings (IBL scale)
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
                                      glm::vec3(0.0F, 0.0F, 1.0F), {}, {}, scene.sky, scene.ibl,
                                      VK_NULL_HANDLE, VK_NULL_HANDLE, {}, scene.tuning);
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

// Issue #45: a mirror-smooth metal seen exactly at the sun's reflection has a
// GGX peak of 1 / (pi * alpha^2) ~ 3e5, beyond half-float range. Writing it
// to the RGBA16F HDR target gives +Inf on drivers that round overflow to
// infinity, and bloom / TAA then turn that Inf into a NaN block. Every lit
// pixel must stay finite and within half range.
TEST(GpuLighting, MirrorHighlightStaysWithinHalfFloatRange) {
    QUAT_REQUIRE_GPU(g);
    Scene scene;
    scene.material.base_color_factor = {1.0F, 1.0F, 1.0F, 1.0F};
    scene.material.metallic_factor = 1.0F;
    scene.material.roughness_factor = 0.0F;
    scene.light = head_on_light();
    scene.light.color.w = 3.0F; // the showcase sun's intensity
    scene.half_size = 0.9F;
    // Aim the light at the centre pixel's mirror direction (camera at +Z,
    // identity view_proj, as LitPixelMatchesThePbrReference reconstructs it)
    // so that pixel sits exactly on the GGX peak: H = N.
    const float u = (static_cast<float>(k_extent.width / 2) + 0.5F) /
                    static_cast<float>(k_extent.width);
    const float v = (static_cast<float>(k_extent.height / 2) + 0.5F) /
                    static_cast<float>(k_extent.height);
    const glm::vec3 surface(u * 2.0F - 1.0F, v * 2.0F - 1.0F, 0.0F);
    const glm::vec3 view = glm::normalize(glm::vec3(0.0F, 0.0F, 1.0F) - surface);
    const glm::vec3 mirror = glm::reflect(-view, glm::vec3(0.0F, 0.0F, 1.0F));
    scene.light.direction = glm::vec4(mirror, 0.0F);
    auto hdr = render_lit(*g, scene);
    ASSERT_OK(hdr);
    float peak = 0.0F;
    for (std::uint32_t y = 0; y < k_extent.height; ++y) {
        for (std::uint32_t x = 0; x < k_extent.width; ++x) {
            const glm::vec4 c = hdr->rgba16f(x, y);
            for (int k = 0; k < 3; ++k) {
                ASSERT_TRUE(std::isfinite(c[k])) << "pixel " << x << "," << y << " ch " << k;
                peak = std::max(peak, c[k]);
            }
        }
    }
    // Strictly below the half-float maximum: drivers that saturate an
    // overflow (instead of rounding it to +Inf) would otherwise hide it here.
    EXPECT_LT(peak, 65504.0F) << "the highlight overflowed the HDR target";
    EXPECT_GT(peak, 100.0F) << "the highlight should still be very bright";
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

// ---- Shadow tuning (A8 settings on the R12 cascades) --------------------------
//
// A screen-filling dielectric receiver at depth 0.5 with an identity view, and
// a hand-built single cascade whose matrix maps world (x, y) straight onto
// shadow-map uv and world z onto depth 1 - z: shadow-map texel (x, y) is
// screen pixel (x, y), the receiver's depth is 0.5, and offsets toward +z (the
// light) lower it. The map is cleared to `left_depth` for x < k_extent.width
// / 2 and `right_depth` elsewhere: a depth below 0.5 occludes the receiver, 1
// leaves it lit. Ambient is off, so the HDR value is the direct light times
// the shadow factor. CsmSettings carries the A8 knobs (PCF radius, texel-unit
// biases); PCSS is off so the radius alone sets the filter.
namespace {

struct ShadowScene {
    float          left_depth = 0.25F;
    float          right_depth = 1.0F;
    glm::vec3      light_to{0.0F, 0.0F, 1.0F}; // direction to the light (shading)
    bool           shadow_map = true;            // false: shadows disabled
    r::CsmSettings csm = [] {
        r::CsmSettings c;
        c.light_size_deg = 0.0F; // fixed-radius PCF
        return c;
    }();
};

// The single cascade described above (2 world units span the 64-texel map).
r::GpuCascades identity_cascade() {
    r::GpuCascades c{};
    glm::mat4 m(1.0F);
    m[2][2] = -1.0F; // depth = 1 - z
    m[3][2] = 1.0F;
    c.view_proj.fill(m);
    c.split_far = glm::vec4(1.0e6F);
    c.texel_world = glm::vec4(2.0F / static_cast<float>(k_extent.width));
    c.depth_range = glm::vec4(1.0F);
    c.params = glm::vec4(1.0F, 0.0F, 0.0F, 0.0F); // one cascade, no blend band
    c.limits = glm::vec4(1.0e6F, 0.0F, 0.0F, 0.0F);
    return c;
}

// The hand-built cascaded map + cascade buffer (persistent, imported).
struct HandMap {
    rhi::GpuImage  image;
    rhi::ImageView view;
    rhi::GpuBuffer cascades;
};

std::expected<HandMap, Error> make_hand_map(quat::test::GpuContext& g) {
    constexpr VkFormat format = r::shadow_map_format_padded;
    HandMap out;
    auto image = g.allocator.create_image(
        format, k_extent, VK_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT | VK_IMAGE_USAGE_SAMPLED_BIT);
    if (!image) return std::unexpected(image.error());
    out.image = std::move(*image);
    auto view = rhi::create_image_view(g.device.handle(), out.image.handle(), format,
                                       VK_IMAGE_ASPECT_DEPTH_BIT, 1, VK_IMAGE_VIEW_TYPE_2D_ARRAY,
                                       0, 0, 1);
    if (!view) return std::unexpected(view.error());
    out.view = std::move(*view);
    auto buffer = g.allocator.create_buffer(
        sizeof(r::GpuCascades),
        VK_BUFFER_USAGE_STORAGE_BUFFER_BIT | VK_BUFFER_USAGE_SHADER_DEVICE_ADDRESS_BIT,
        VMA_MEMORY_USAGE_AUTO,
        VMA_ALLOCATION_CREATE_HOST_ACCESS_SEQUENTIAL_WRITE_BIT | VMA_ALLOCATION_CREATE_MAPPED_BIT);
    if (!buffer) return std::unexpected(buffer.error());
    out.cascades = std::move(*buffer);
    const r::GpuCascades data = identity_cascade();
    std::memcpy(out.cascades.mapped(), &data, sizeof(data));
    if (auto f = out.cascades.flush(); !f) return std::unexpected(f.error());
    return out;
}

std::expected<HostImage, Error> render_shadowed(quat::test::GpuContext& g, const ShadowScene& sc) {
    constexpr float s = 4.0F; // covers the whole clip square
    std::array<asset::Vertex, 3> verts{};
    verts[0].position = {-s, -s, 0.5F};
    verts[1].position = {s, -s, 0.5F};
    verts[2].position = {0.0F, s, 0.5F};
    const std::array<std::uint32_t, 3> indices{0, 1, 2};
    auto mesh = quat::test::upload_mesh(g, verts, indices);
    if (!mesh) return std::unexpected(mesh.error());
    asset::PbrMaterialParams params;
    params.base_color_factor = {0.5F, 0.5F, 0.5F, 1.0F};
    params.metallic_factor = 0.0F;
    params.roughness_factor = 1.0F;
    auto material = asset::upload_material(params, asset::MaterialTextures{}, g.allocator,
                                           g.transfer);
    if (!material) return std::unexpected(material.error());
    auto mesh_pass = r::MeshPass::create(g.device, g.allocator, g.cache, g.transfer, g.shader_dir);
    if (!mesh_pass) return std::unexpected(mesh_pass.error());
    auto lighting = r::LightingPass::create(g.device, g.allocator, g.cache, g.shader_dir);
    if (!lighting) return std::unexpected(lighting.error());
    auto hand = make_hand_map(g);
    if (!hand) return std::unexpected(hand.error());

    rhi::TransientImagePool pool(g.device, g.allocator);
    rhi::RenderGraph graph(pool);
    const r::DrawItem item{&*mesh, &*material, glm::mat4(1.0F)};
    auto targets = mesh_pass->add_to_graph(graph, k_extent, glm::mat4(1.0F), {&item, 1});
    if (!targets) return std::unexpected(targets.error());

    r::LightingOcclusion occlusion;
    occlusion.csm = sc.csm;
    if (sc.shadow_map) {
        const rhi::ResourceHandle map = graph.import_image(
            "test_shadow_map", hand->image.handle(), hand->view.handle(),
            r::shadow_map_format_padded, k_extent, VK_IMAGE_LAYOUT_UNDEFINED,
            VK_IMAGE_LAYOUT_UNDEFINED);
        graph.add_pass("test_shadow_fill", rhi::PassType::graphics)
            .writes(map, rhi::ResourceUsage::depth_attachment)
            .execute([map, sc](rhi::PassContext& ctx) {
                VkRenderingAttachmentInfo depth{};
                depth.sType = VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO;
                depth.imageView = ctx.resolve(map).view;
                depth.imageLayout = VK_IMAGE_LAYOUT_DEPTH_ATTACHMENT_OPTIMAL;
                depth.loadOp = VK_ATTACHMENT_LOAD_OP_CLEAR;
                depth.storeOp = VK_ATTACHMENT_STORE_OP_STORE;
                depth.clearValue.depthStencil = {sc.right_depth, 0};
                VkRenderingInfo info{};
                info.sType = VK_STRUCTURE_TYPE_RENDERING_INFO;
                info.renderArea = {{0, 0}, k_extent};
                info.layerCount = 1;
                info.pDepthAttachment = &depth;
                vkCmdBeginRendering(ctx.cmd(), &info);
                VkClearAttachment left{};
                left.aspectMask = VK_IMAGE_ASPECT_DEPTH_BIT;
                left.clearValue.depthStencil = {sc.left_depth, 0};
                const VkClearRect rect{{{0, 0}, {k_extent.width / 2, k_extent.height}}, 0, 1};
                vkCmdClearAttachments(ctx.cmd(), 1, &left, 1, &rect);
                vkCmdEndRendering(ctx.cmd());
            });
        occlusion.shadows.shadow_map = map;
        occlusion.shadows.cascades = graph.import_buffer(
            "test_cascades", hand->cascades.handle(), sizeof(r::GpuCascades));
        occlusion.shadows.cascades_address =
            rhi::buffer_device_address(g.device.handle(), hand->cascades);
    }
    r::DirectionalLightParams light = head_on_light();
    light.direction = glm::vec4(glm::normalize(sc.light_to), 0.0F);
    auto hdr = lighting->add_to_graph(graph, *targets, k_extent, light, glm::mat4(1.0F),
                                      glm::vec3(0.0F, 0.0F, 1.0F), occlusion);
    if (!hdr) return std::unexpected(hdr.error());
    return quat::test::render_to_host(g, graph, *hdr, k_extent, 8);
}

// Shadow factor of pixel (x, centre row): its value over the same pixel
// rendered without a shadow map (the view-dependent specular varies across the
// receiver, so each pixel is compared with itself).
[[nodiscard]] float shadow_at(const HostImage& shadowed, const HostImage& unshadowed,
                              std::uint32_t x) {
    const std::uint32_t y = k_extent.height / 2;
    return shadowed.rgba16f(x, y).r / unshadowed.rgba16f(x, y).r;
}

// The same scene without a shadow map.
std::expected<HostImage, Error> render_unshadowed(quat::test::GpuContext& g, ShadowScene sc) {
    sc.shadow_map = false;
    return render_shadowed(g, sc);
}

// Pixels of the centre row in the penumbra (neither fully lit nor shadowed).
[[nodiscard]] int penumbra_width(const HostImage& shadowed, const HostImage& unshadowed) {
    int n = 0;
    for (std::uint32_t x = 0; x < k_extent.width; ++x) {
        const float v = shadow_at(shadowed, unshadowed, x);
        n += v > 0.02F && v < 0.98F ? 1 : 0;
    }
    return n;
}

} // namespace

// PCF radius (CsmSettings::pcf_radius, texels): r = 0 is one hardware 2x2
// comparison tap, a penumbra about a texel wide at the edge; larger radii
// widen it (a noise-rotated Vogel disk of comparison taps). Fully lit and
// deep-shadow pixels stay 1 and 0 for every radius.
TEST(GpuLighting, PcfRadiusWidensThePenumbra) {
    QUAT_REQUIRE_GPU(g);
    auto reference = render_unshadowed(*g, ShadowScene{});
    ASSERT_OK(reference);
    ASSERT_GT(reference->rgba16f(k_extent.width / 2, k_extent.height / 2).r, 0.05F);
    int previous = -1;
    for (const float radius : {0.0F, 1.5F, 3.0F, 6.0F}) {
        ShadowScene sc;
        sc.csm.pcf_radius = radius;
        auto hdr = render_shadowed(*g, sc);
        ASSERT_OK(hdr);
        const auto at = [&](std::uint32_t x) { return shadow_at(*hdr, *reference, x); };
        EXPECT_NEAR(at(k_extent.width - 8), 1.0F, 1e-3F) << "r=" << radius;
        EXPECT_NEAR(at(4), 0.0F, 1e-3F) << "r=" << radius;
        const int width = penumbra_width(*hdr, *reference);
        if (radius == 0.0F) {
            EXPECT_LE(width, 2) << "hard edge";
        } else {
            EXPECT_GT(width, previous) << "r=" << radius;
            EXPECT_LE(width, static_cast<int>(2.0F * radius) + 3) << "r=" << radius;
        }
        previous = width;
    }
}

// Bias sweep: an occluder 0.01 in front of the receiver (self-shadowing acne)
// shadows it with no receiver offsets; the default texel-unit offsets (normal
// and depth bias, 1 texel each of 1/32 world unit) clear it, as does either
// one alone; offsets of a tenth of a texel do not. Lit at 60 degrees
// (N.L = 0.5).
TEST(GpuLighting, BiasRemovesSelfShadowing) {
    QUAT_REQUIRE_GPU(g);
    ShadowScene sc;
    sc.left_depth = 0.49F;
    sc.light_to = glm::vec3(0.0F, std::sqrt(3.0F) / 2.0F, 0.5F);
    sc.csm.pcf_radius = 0.0F;
    auto reference = render_unshadowed(*g, sc);
    ASSERT_OK(reference);
    const auto acne = [&](float normal_bias, float depth_bias) -> std::expected<float, Error> {
        ShadowScene t = sc;
        t.csm.normal_bias = normal_bias;
        t.csm.depth_bias = depth_bias;
        auto hdr = render_shadowed(*g, t);
        if (!hdr) return std::unexpected(hdr.error());
        return shadow_at(*hdr, *reference, 8);
    };
    auto none = acne(0.0F, 0.0F);
    ASSERT_OK(none);
    EXPECT_NEAR(*none, 0.0F, 1e-3F) << "no offsets: the receiver shadows itself";
    const r::CsmSettings defaults;
    auto both = acne(defaults.normal_bias, defaults.depth_bias);
    ASSERT_OK(both);
    EXPECT_NEAR(*both, 1.0F, 1e-3F) << "default offsets";
    auto normal = acne(1.0F, 0.0F); // 0.75 texel toward the light along N
    ASSERT_OK(normal);
    EXPECT_NEAR(*normal, 1.0F, 1e-3F) << "normal bias alone";
    auto depth = acne(0.0F, 1.0F); // 1/32 * 0.5 = 0.0156 > 0.01 toward the light
    ASSERT_OK(depth);
    EXPECT_NEAR(*depth, 1.0F, 1e-3F) << "depth bias alone";
    auto small = acne(0.1F, 0.1F);
    ASSERT_OK(small);
    EXPECT_NEAR(*small, 0.0F, 1e-3F) << "a tenth of a texel keeps the self-shadowing";
}

// shadows.enabled = false: the frame graph adds no shadow passes and hands
// the lighting pass no cascades — the shadowed half lights up like the lit
// half (same shading as with an all-lit shadow map).
TEST(GpuLighting, NoShadowMapLightsEverything) {
    QUAT_REQUIRE_GPU(g);
    ShadowScene all_lit;
    all_lit.left_depth = 1.0F;
    auto lit = render_shadowed(*g, all_lit);
    ASSERT_OK(lit);
    auto with = render_shadowed(*g, ShadowScene{});
    ASSERT_OK(with);
    auto without = render_unshadowed(*g, ShadowScene{});
    ASSERT_OK(without);
    EXPECT_NEAR(shadow_at(*with, *lit, 8), 0.0F, 1e-3F);
    for (const std::uint32_t x : {8U, k_extent.width / 2 - 1, k_extent.width - 8}) {
        EXPECT_NEAR(shadow_at(*without, *lit, x), 1.0F, 1e-3F) << "x=" << x;
    }
}

// With the environment disabled (and no direct light) the metal is black.
TEST(GpuIblLighting, NoEnvironmentAndNoLightIsBlack) {
    QUAT_REQUIRE_GPU(g);
    auto hdr = render_lit(*g, environment_only_metal());
    ASSERT_OK(hdr);
    const glm::vec3 lit = centre_rgb(*hdr);
    EXPECT_LE(lit.r + lit.g + lit.b, 0.02F);
}
