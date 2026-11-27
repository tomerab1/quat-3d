// Weather in the deferred lighting pass (R13.1): Lagarde's wet surfaces and
// puddles, snow cover, and cloud shadows on the sun — on a flat floor (normal
// +Y) seen from above, read back from the HDR target.

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <expected>
#include <optional>
#include <vector>

#include <glm/glm.hpp>
#include <gtest/gtest.h>
#include <vulkan/vulkan.h>

#include "engine/asset/material_asset.hpp"
#include "engine/asset/mesh_asset.hpp"
#include "engine/renderer/atmosphere_pass.hpp"
#include "engine/renderer/cloud_settings.hpp"
#include "engine/renderer/lighting_pass.hpp"
#include "engine/renderer/mesh_pass.hpp"
#include "engine/renderer/weather_surfaces.hpp"
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
constexpr float      k_half_m = 20.0F; // the floor spans [-20, 20] m in X and Z

// World -> clip for a camera looking straight down: clip.x = x / 20,
// clip.y = z / 20, depth = 0.5 + y / 100 (invertible, so the lighting pass
// reconstructs world positions on the floor at y = 0).
[[nodiscard]] glm::mat4 top_down_view_proj() {
    glm::mat4 m(0.0F);
    m[0] = {1.0F / k_half_m, 0.0F, 0.0F, 0.0F};
    m[1] = {0.0F, 0.0F, 0.01F, 0.0F};
    m[2] = {0.0F, 1.0F / k_half_m, 0.0F, 0.0F};
    m[3] = {0.0F, 0.0F, 0.5F, 1.0F};
    return m;
}

const glm::vec3 k_sun = glm::normalize(glm::vec3(0.3F, 1.0F, 0.2F)); // toward the sun

struct Floor {
    float                 albedo = 0.5F;
    float                 roughness = 0.9F;
    glm::vec3             camera{0.0F, 50.0F, 0.0F};
    r::WeatherSurfaces    weather{};
    bool                  sky = false; // atmosphere LUTs + clouds
    r::CloudSettings      clouds{};
};

std::expected<HostImage, Error> render_floor(quat::test::GpuContext& g, const Floor& f) {
    std::array<asset::Vertex, 4> verts{};
    const float s = k_half_m;
    verts[0].position = {-s, 0.0F, -s};
    verts[1].position = {s, 0.0F, -s};
    verts[2].position = {s, 0.0F, s};
    verts[3].position = {-s, 0.0F, s};
    for (asset::Vertex& v : verts) {
        v.normal = {0.0F, 1.0F, 0.0F};
        v.tangent = {1.0F, 0.0F, 0.0F, 1.0F};
    }
    const std::array<std::uint32_t, 6> indices{0, 1, 2, 0, 2, 3};
    auto mesh = quat::test::upload_mesh(g, verts, indices);
    if (!mesh) return std::unexpected(mesh.error());
    asset::PbrMaterialParams params;
    params.base_color_factor = {glm::vec3(f.albedo), 1.0F};
    params.metallic_factor = 0.0F;
    params.roughness_factor = f.roughness;
    auto material = asset::upload_material(params, asset::MaterialTextures{}, g.allocator, g.transfer);
    if (!material) return std::unexpected(material.error());
    auto mesh_pass = r::MeshPass::create(g.device, g.allocator, g.cache, g.transfer, g.shader_dir);
    if (!mesh_pass) return std::unexpected(mesh_pass.error());
    auto lighting = r::LightingPass::create(g.device, g.allocator, g.cache, g.shader_dir);
    if (!lighting) return std::unexpected(lighting.error());
    std::optional<r::AtmospherePass> atmosphere;
    if (f.sky) {
        auto a = r::AtmospherePass::create(g.device, g.allocator, g.cache, g.shader_dir);
        if (!a) return std::unexpected(a.error());
        if (auto ok = a->ensure_skyview(k_sun); !ok) return std::unexpected(ok.error());
        atmosphere = std::move(*a);
    }

    r::DirectionalLightParams light;
    light.direction = glm::vec4(k_sun, 0.0F);
    light.color = {1.0F, 1.0F, 1.0F, 1.0F};
    light.ambient = {0.0F, 0.0F, 0.0F, 0.0F};
    r::LightingTuning tuning;
    tuning.weather = f.weather;
    tuning.ibl_intensity = 0.0F; // direct sunlight only

    rhi::TransientImagePool pool(g.device, g.allocator);
    rhi::RenderGraph graph(pool);
    const glm::mat4 vp = top_down_view_proj();
    const r::DrawItem item{&*mesh, &*material, glm::mat4(1.0F)};
    auto targets = mesh_pass->add_to_graph(graph, k_extent, vp, {&item, 1});
    if (!targets) return std::unexpected(targets.error());
    auto hdr = lighting->add_to_graph(
        graph, *targets, k_extent, light, glm::inverse(vp), f.camera, {}, {}, f.sky, {},
        atmosphere ? atmosphere->skyview_view() : VK_NULL_HANDLE,
        atmosphere ? atmosphere->transmittance_view() : VK_NULL_HANDLE, f.clouds, tuning);
    if (!hdr) return std::unexpected(hdr.error());
    return quat::test::render_to_host(g, graph, *hdr, k_extent, 8);
}

// World position of pixel (x, y)'s centre on the floor.
[[nodiscard]] glm::vec3 floor_point(std::uint32_t x, std::uint32_t y) {
    const glm::vec2 ndc = (glm::vec2(static_cast<float>(x), static_cast<float>(y)) + 0.5F) /
                              glm::vec2(k_extent.width, k_extent.height) * 2.0F -
                          1.0F;
    return {ndc.x * k_half_m, 0.0F, ndc.y * k_half_m};
}

[[nodiscard]] float q8(float v) { return std::round(v * 255.0F) / 255.0F; }

[[nodiscard]] float luminance(const glm::vec4& c) {
    return 0.2126F * c.r + 0.7152F * c.g + 0.0722F * c.b;
}

[[nodiscard]] std::vector<float> luminances(const HostImage& img) {
    std::vector<float> out;
    for (std::uint32_t y = 0; y < img.extent.height; ++y) {
        for (std::uint32_t x = 0; x < img.extent.width; ++x) out.push_back(luminance(img.rgba16f(x, y)));
    }
    return out;
}

[[nodiscard]] float mean(const std::vector<float>& v) {
    double s = 0.0;
    for (const float f : v) s += f;
    return static_cast<float>(s / static_cast<double>(v.size()));
}

void expect_rgb_near(const glm::vec3& got, const glm::vec3& want, float rel) {
    for (int i = 0; i < 3; ++i) {
        EXPECT_NEAR(got[i], want[i], rel * want[i] + 1e-4F) << "channel " << i;
    }
}

TEST(GpuWeatherLighting, WetPorousSurfaceDarkensAndGlossesLikeLagarde) {
    QUAT_REQUIRE_GPU(g);
    Floor dry;
    auto dry_img = render_floor(*g, dry);
    ASSERT_OK(dry_img);
    Floor wet = dry;
    wet.weather.wetness = 0.5F; // below the puddle threshold
    auto wet_img = render_floor(*g, wet);
    ASSERT_OK(wet_img);

    const glm::uvec2 c = dry_img->centre();
    const glm::vec3 p = floor_point(c.x, c.y);
    const glm::vec3 N(0.0F, 1.0F, 0.0F);
    const glm::vec3 V = glm::normalize(dry.camera - p);
    const float albedo = q8(dry.albedo);
    const float rough = q8(dry.roughness);
    namespace pbr = quat::test::pbr_ref;
    const glm::vec3 want_dry =
        pbr::direct(glm::vec3(albedo), 0.0F, rough, N, V, k_sun, glm::vec3(1.0F), 1.0F);
    // Lagarde: porosity 1 (rough dielectric) -> diffuse x lerp(1, 0.2, w),
    // gloss = lerp(1, gloss, lerp(1, 0.2, w / 2)).
    const float porosity = std::clamp((rough - 0.5F) / 0.4F, 0.0F, 1.0F);
    const float factor = 1.0F + (0.2F - 1.0F) * porosity;
    const float w = 0.5F;
    const float gloss = 1.0F + ((1.0F - rough) - 1.0F) * (1.0F + (factor - 1.0F) * 0.5F * w);
    const glm::vec3 want_wet = pbr::direct(glm::vec3(albedo * (1.0F + (factor - 1.0F) * w)), 0.0F,
                                           1.0F - gloss, N, V, k_sun, glm::vec3(1.0F), 1.0F);
    expect_rgb_near(glm::vec3(dry_img->rgba16f(c.x, c.y)), want_dry, 0.02F);
    expect_rgb_near(glm::vec3(wet_img->rgba16f(c.x, c.y)), want_wet, 0.02F);
    EXPECT_LT(want_wet.g, 0.75F * want_dry.g);
}

TEST(GpuWeatherLighting, SoakedFlatGroundGrowsMirrorPuddles) {
    QUAT_REQUIRE_GPU(g);
    // Camera in the sun's mirror direction: still water glints, wet ground
    // does not.
    Floor f;
    f.camera = glm::vec3(-k_sun.x, k_sun.y, -k_sun.z) * 2000.0F;
    f.weather.wetness = 0.5F;
    auto damp = render_floor(*g, f);
    ASSERT_OK(damp);
    f.weather.wetness = 1.0F;
    auto soaked = render_floor(*g, f);
    ASSERT_OK(soaked);

    const std::vector<float> damp_l = luminances(*damp);
    const float damp_max = *std::max_element(damp_l.begin(), damp_l.end());
    std::size_t glints = 0;
    const std::vector<float> soaked_l = luminances(*soaked);
    for (const float l : soaked_l) glints += l > 10.0F * damp_max ? 1U : 0U;
    const float fraction = static_cast<float>(glints) / static_cast<float>(soaked_l.size());
    EXPECT_GT(fraction, 0.05F) << "puddles appear on soaked flat ground";
    EXPECT_LT(fraction, 0.97F) << "puddles are patches, not a lake";
}

TEST(GpuWeatherLighting, SnowCoversUpwardFacingGround) {
    QUAT_REQUIRE_GPU(g);
    Floor f;
    f.albedo = 0.1F;
    f.weather.snow = 1.0F;
    auto img = render_floor(*g, f);
    ASSERT_OK(img);
    const glm::uvec2 c = img->centre();
    const glm::vec3 p = floor_point(c.x, c.y);
    const glm::vec3 N(0.0F, 1.0F, 0.0F);
    const glm::vec3 want = quat::test::pbr_ref::direct(
        glm::vec3(0.83F, 0.86F, 0.90F), 0.0F, 0.55F, N, glm::normalize(f.camera - p), k_sun,
        glm::vec3(1.0F), 1.0F);
    expect_rgb_near(glm::vec3(img->rgba16f(c.x, c.y)), want, 0.02F);
}

TEST(GpuWeatherLighting, SnowLiesDeeperAboveTheSnowline) {
    QUAT_REQUIRE_GPU(g);
    Floor f;
    f.albedo = 0.1F;
    f.weather.snow = 0.03F; // a dusting
    f.weather.snowline_m = r::k_no_snowline_m;
    auto valley = render_floor(*g, f);
    ASSERT_OK(valley);
    f.weather.snowline_m = -500.0F; // the floor is far above the snowline
    auto peak = render_floor(*g, f);
    ASSERT_OK(peak);
    EXPECT_GT(mean(luminances(*peak)), 1.3F * mean(luminances(*valley)));
}

TEST(GpuWeatherLighting, CloudsShadowTheSun) {
    QUAT_REQUIRE_GPU(g);
    Floor f;
    f.sky = true;
    f.clouds.coverage = 1.0F;
    f.clouds.density = 60.0F;
    f.clouds.shadows = false;
    auto lit = render_floor(*g, f);
    ASSERT_OK(lit);
    f.clouds.shadows = true;
    auto shaded = render_floor(*g, f);
    ASSERT_OK(shaded);
    f.clouds.coverage = 0.0F;
    auto clear = render_floor(*g, f);
    ASSERT_OK(clear);

    const float lit_l = mean(luminances(*lit));
    EXPECT_GT(lit_l, 0.0F);
    EXPECT_LT(mean(luminances(*shaded)), 0.7F * lit_l) << "an overcast sky shadows the sun";
    EXPECT_NEAR(mean(luminances(*clear)), lit_l, 0.01F * lit_l) << "no clouds, no shadow";
}

} // namespace
