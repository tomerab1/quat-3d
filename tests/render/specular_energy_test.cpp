// White furnace tests of the lighting pass (R21): a white surface inside a
// uniform white environment (an HdriEnvironment baked from a constant
// panorama, radiance 1) must reflect exactly the light it receives, 1, at
// every roughness and view angle. Single-scattering GGX loses the energy of
// light bouncing between microfacets — rough metals render dark — and the
// multiple-scattering compensation (lib/brdf_energy.slang) restores it. The
// sun is off, so the pixel is the image-based term alone.

#include <array>
#include <cmath>
#include <cstdint>
#include <expected>
#include <optional>
#include <span>

#include <glm/glm.hpp>
#include <glm/gtc/constants.hpp>
#include <gtest/gtest.h>
#include <vulkan/vulkan.h>

#include "engine/asset/hdr_decode.hpp"
#include "engine/asset/material_asset.hpp"
#include "engine/asset/mesh_asset.hpp"
#include "engine/renderer/hdri_environment.hpp"
#include "engine/renderer/lighting_pass.hpp"
#include "engine/renderer/mesh_pass.hpp"
#include "engine/rhi/render_graph.hpp"
#include "support/expect_ok.hpp"
#include "support/gpu_context.hpp"
#include "support/gpu_submit.hpp"
#include "support/render_readback.hpp"

namespace {

namespace r = engine::renderer;
namespace rhi = engine::rhi;
namespace asset = engine::asset;
using engine::core::Error;

constexpr VkExtent2D k_extent{32, 32};

struct Furnace {
    std::optional<r::HdriEnvironment> env;
    std::optional<r::MeshPass>        mesh_pass;
    std::optional<r::LightingPass>    lighting;
};

std::expected<Furnace, Error> make_furnace(quat::test::GpuContext& g) {
    Furnace f;
    asset::HdrImage white;
    white.width = 64;
    white.height = 32;
    white.pixels = asset::PixelBuffer::allocate(asset::hdr_decoded_bytes(64, 32));
    auto* p = reinterpret_cast<float*>(white.pixels.data());
    for (std::size_t i = 0; i < 64 * 32 * 3; ++i) p[i] = 1.0F;
    auto env = r::HdriEnvironment::create(g.device, g.allocator, g.cache, g.shader_dir);
    if (!env) return std::unexpected(env.error());
    f.env.emplace(std::move(*env));
    if (auto s = f.env->set_image(white, g.transfer); !s) return std::unexpected(s.error());
    rhi::RenderGraph bake;
    if (auto b = f.env->add_bake_to_graph(bake, {}); !b) return std::unexpected(b.error());
    if (auto c = bake.compile(); !c) return std::unexpected(c.error());
    auto ran =
        quat::test::submit_and_wait(g.device, [&](VkCommandBuffer cmd) { bake.execute(cmd); });
    if (!ran) return std::unexpected(ran.error());
    auto mesh_pass = r::MeshPass::create(g.device, g.allocator, g.cache, g.transfer, g.shader_dir);
    if (!mesh_pass) return std::unexpected(mesh_pass.error());
    f.mesh_pass.emplace(std::move(*mesh_pass));
    auto lighting = r::LightingPass::create(g.device, g.allocator, g.cache, g.shader_dir);
    if (!lighting) return std::unexpected(lighting.error());
    f.lighting.emplace(std::move(*lighting));
    return f;
}

// A camera-facing quad (identity view-projection, camera at +Z) whose
// shading normal is tilted `tilt_deg` off the view direction, so the centre
// pixel is shaded at NdotV = cos(tilt).
std::expected<glm::vec3, Error> furnace_pixel(quat::test::GpuContext& g, Furnace& f,
                                              float metallic, float roughness, float tilt_deg,
                                              bool multiscatter, bool horizon_occlusion = false) {
    const float t = glm::radians(tilt_deg);
    const glm::vec3 n(std::sin(t), 0.0F, std::cos(t));
    std::array<asset::Vertex, 4> verts{};
    const std::array<glm::vec2, 4> corners{{{-1, -1}, {1, -1}, {1, 1}, {-1, 1}}};
    for (std::size_t i = 0; i < 4; ++i) {
        verts[i].position = {corners[i] * 0.9F, 0.0F};
        verts[i].normal = n;
    }
    // Counter-clockwise in framebuffer space (the identity projection keeps
    // Vulkan's y-down NDC), so the quad is a front face, and single-sided, so
    // it is shaded with `n` as given. A double-sided back face is shaded with
    // -n (glTF doubleSided, #70): facing away from the camera, at NdotV ~ 0,
    // which the furnaces with multiple scattering cannot tell apart (they
    // reach 1 at any angle) but SingleScatteringLosesEnergyOnRoughMetal can.
    const std::array<std::uint32_t, 6> indices{0, 2, 1, 0, 3, 2};
    auto mesh = quat::test::upload_mesh(g, verts, indices);
    if (!mesh) return std::unexpected(mesh.error());
    asset::PbrMaterialParams params;
    params.base_color_factor = {1.0F, 1.0F, 1.0F, 1.0F};
    params.metallic_factor = metallic;
    params.roughness_factor = roughness;
    auto material = asset::upload_material(params, asset::MaterialTextures{}, g.allocator,
                                           g.transfer);
    if (!material) return std::unexpected(material.error());

    rhi::TransientImagePool pool(g.device, g.allocator);
    rhi::RenderGraph graph(pool);
    const r::DrawItem item{&*mesh, &*material, glm::mat4(1.0F)};
    auto targets = f.mesh_pass->add_to_graph(graph, k_extent, glm::mat4(1.0F), {&item, 1});
    if (!targets) return std::unexpected(targets.error());
    r::DirectionalLightParams light;
    light.color = {1.0F, 1.0F, 1.0F, 0.0F}; // sun off
    light.ambient = {0.0F, 0.0F, 0.0F, 0.0F};
    r::LightingTuning tuning;
    tuning.multiscatter = multiscatter;
    tuning.horizon_occlusion = horizon_occlusion; // off: the furnace measures the BRDF alone
    auto hdr = f.lighting->add_to_graph(graph, *targets, k_extent, light, glm::mat4(1.0F),
                                        glm::vec3(0.0F, 0.0F, 1.0F), {}, {}, /*enable_sky=*/true,
                                        f.env->views(), VK_NULL_HANDLE, VK_NULL_HANDLE, {},
                                        tuning);
    if (!hdr) return std::unexpected(hdr.error());
    auto image = quat::test::render_to_host(g, graph, *hdr, k_extent, 8);
    if (!image) return std::unexpected(image.error());
    const glm::uvec2 c = image->centre();
    return glm::vec3(image->rgba16f(c.x, c.y));
}

constexpr std::array<float, 6> k_roughness{0.05F, 0.2F, 0.4F, 0.6F, 0.8F, 1.0F};

} // namespace

// White metal (F0 = 1): every unit of light that reaches it leaves it.
TEST(GpuSpecularEnergy, WhiteFurnaceMetalKeepsItsEnergy) {
    QUAT_REQUIRE_GPU(g);
    // Per test, not static: GPU objects must die before the shared device.
    auto f = make_furnace(*g);
    ASSERT_OK(f);
    for (const float tilt : {0.0F, 45.0F, 70.0F}) {
        for (const float rough : k_roughness) {
            auto px = furnace_pixel(*g, *f, 1.0F, rough, tilt, true);
            ASSERT_OK(px);
            EXPECT_NEAR(px->g, 1.0F, 0.02F) << "roughness " << rough << " tilt " << tilt;
        }
    }
}

// White dielectric: what the specular lobes do not reflect, diffuse does.
TEST(GpuSpecularEnergy, WhiteFurnaceDielectricKeepsItsEnergy) {
    QUAT_REQUIRE_GPU(g);
    // Per test, not static: GPU objects must die before the shared device.
    auto f = make_furnace(*g);
    ASSERT_OK(f);
    for (const float tilt : {0.0F, 60.0F}) {
        for (const float rough : k_roughness) {
            auto px = furnace_pixel(*g, *f, 0.0F, rough, tilt, true);
            ASSERT_OK(px);
            EXPECT_NEAR(px->g, 1.0F, 0.02F) << "roughness " << rough << " tilt " << tilt;
        }
    }
}

// Without the compensation a rough metal loses a large share of its energy
// (the defect the multiple-scattering term fixes), a smooth one almost none.
TEST(GpuSpecularEnergy, SingleScatteringLosesEnergyOnRoughMetal) {
    QUAT_REQUIRE_GPU(g);
    // Per test, not static: GPU objects must die before the shared device.
    auto f = make_furnace(*g);
    ASSERT_OK(f);
    auto smooth = furnace_pixel(*g, *f, 1.0F, 0.05F, 0.0F, false);
    ASSERT_OK(smooth);
    EXPECT_GT(smooth->g, 0.97F);
    auto rough = furnace_pixel(*g, *f, 1.0F, 1.0F, 0.0F, false);
    ASSERT_OK(rough);
    EXPECT_LT(rough->g, 0.9F);
    auto compensated = furnace_pixel(*g, *f, 1.0F, 1.0F, 0.0F, true);
    ASSERT_OK(compensated);
    EXPECT_GT(compensated->g, rough->g + 0.08F);
}

// Horizon occlusion (Russell 2014 / Jimenez et al. 2016, lib/brdf_energy.slang):
// the quad's geometric normal (rebuilt from depth) faces the camera, but its
// shading normal is tilted 70 degrees, as a normal map would, so the mirror
// direction reflect(-v, n) points 50 degrees below the surface. That light is
// blocked by the surface itself, and the specular must lose it; with the
// shading normal untilted the reflection stays above the horizon and the
// term changes nothing. Single scattering on a smooth white metal, so the
// pixel is the occluded specular alone.
TEST(GpuSpecularEnergy, HorizonOcclusionDimsReflectionsBelowTheSurface) {
    QUAT_REQUIRE_GPU(g);
    // Per test, not static: GPU objects must die before the shared device.
    auto f = make_furnace(*g);
    ASSERT_OK(f);
    auto tilted_off = furnace_pixel(*g, *f, 1.0F, 0.2F, 70.0F, false, false);
    auto tilted_on = furnace_pixel(*g, *f, 1.0F, 0.2F, 70.0F, false, true);
    ASSERT_OK(tilted_off);
    ASSERT_OK(tilted_on);
    EXPECT_GT(tilted_off->g, 0.5F); // the unoccluded reflection is bright
    // h = saturate(1 + 1.3 dot(r, ng))^2 = ~0 here: nearly all of it goes.
    EXPECT_LT(tilted_on->g, 0.25F * tilted_off->g)
        << "off " << tilted_off->g << " on " << tilted_on->g;

    // Control: the shading normal matches the geometry, nothing is occluded.
    auto flat_off = furnace_pixel(*g, *f, 1.0F, 0.2F, 0.0F, false, false);
    auto flat_on = furnace_pixel(*g, *f, 1.0F, 0.2F, 0.0F, false, true);
    ASSERT_OK(flat_off);
    ASSERT_OK(flat_on);
    EXPECT_NEAR(flat_on->g, flat_off->g, 0.01F);
}

// The HDRI's sun is clamped out of the environment maps into the
// directional light; the background draws it back as an analytic disc
// carrying that light's irradiance E over its solid angle (radiance
// E / (2 pi (1 - cos r)), r = half the shadows' sun size). An empty GBuffer
// is all background: the furnace's radiance 1 everywhere, plus the disc
// around the sun direction, aimed at the centre pixel (sun size 10 deg, so
// the disc covers it whatever the background depth).
TEST(GpuSpecularEnergy, HdriSunDiscCarriesTheSunsIrradiance) {
    QUAT_REQUIRE_GPU(g);
    // Per test, not static: GPU objects must die before the shared device.
    auto f = make_furnace(*g);
    ASSERT_OK(f);
    constexpr float k_sun_size_deg = 10.0F;
    constexpr float k_irradiance = 2.0F;
    const auto background = [&](bool disc, float sun_size_deg)
        -> std::expected<quat::test::HostImage, Error> {
        rhi::TransientImagePool pool(g->device, g->allocator);
        rhi::RenderGraph graph(pool);
        auto targets = f->mesh_pass->add_to_graph(graph, k_extent, glm::mat4(1.0F),
                                                  std::span<const r::DrawItem>{});
        if (!targets) return std::unexpected(targets.error());
        r::DirectionalLightParams light;
        light.direction = {0.0F, 0.0F, 1.0F, 0.0F}; // towards the sun: straight ahead
        light.color = {1.0F, 1.0F, 1.0F, k_irradiance};
        light.ambient = {0.0F, 0.0F, 0.0F, 0.0F};
        r::LightingOcclusion occlusion;
        occlusion.csm.light_size_deg = sun_size_deg;
        r::LightingTuning tuning;
        tuning.environment = f->env->environment_view();
        tuning.hdri_sun_disc = disc;
        auto hdr = f->lighting->add_to_graph(graph, *targets, k_extent, light, glm::mat4(1.0F),
                                             glm::vec3(0.0F, 0.0F, -1.0F), occlusion, {},
                                             /*enable_sky=*/true, f->env->views(), VK_NULL_HANDLE,
                                             VK_NULL_HANDLE, {}, tuning);
        if (!hdr) return std::unexpected(hdr.error());
        return quat::test::render_to_host(*g, graph, *hdr, k_extent, 8);
    };
    auto with = background(true, k_sun_size_deg);
    auto without = background(false, k_sun_size_deg);
    ASSERT_OK(with);
    ASSERT_OK(without);
    const glm::uvec2 c = with->centre();
    const float r = glm::radians(0.5F * k_sun_size_deg);
    const float expected = 1.0F + k_irradiance / (2.0F * glm::pi<float>() * (1.0F - std::cos(r)));
    EXPECT_NEAR(with->rgba16f(c.x, c.y).g, expected, 0.03F * expected);
    EXPECT_NEAR(without->rgba16f(c.x, c.y).g, 1.0F, 0.02F); // the clamped sky alone
    EXPECT_NEAR(with->rgba16f(0, 0).g, 1.0F, 0.02F);        // far from the sun: no disc

    // A point sun (size 0: hard shadows) still gets a tiny disc, and the
    // frame stays finite: 1 - cos r must not cancel to 0 (issue #45's NaN
    // block starts from one Inf / NaN pixel).
    auto point = background(true, 0.0F);
    ASSERT_OK(point);
    for (std::uint32_t y = 0; y < k_extent.height; ++y) {
        for (std::uint32_t x = 0; x < k_extent.width; ++x) {
            const glm::vec4 p = point->rgba16f(x, y);
            ASSERT_TRUE(std::isfinite(p.r) && std::isfinite(p.g) && std::isfinite(p.b))
                << "pixel " << x << "," << y;
        }
    }
}
