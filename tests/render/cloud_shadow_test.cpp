// The cloud shadow map (R13.1; R13.4 cloud model): its light-space frame on
// the CPU, and the GPU map against the CPU reference of the cloud density
// (cloud_reference over the CPU-baked noise), texel by texel.

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <vector>

#include <glm/glm.hpp>
#include <glm/gtc/packing.hpp>
#include <gtest/gtest.h>

#include "engine/renderer/cloud_noise.hpp"
#include "engine/renderer/cloud_noise_pass.hpp"
#include "engine/renderer/cloud_reference.hpp"
#include "engine/renderer/cloud_settings.hpp"
#include "engine/renderer/cloud_shadow_pass.hpp"
#include "engine/rhi/render_graph.hpp"
#include "support/expect_ok.hpp"
#include "support/gpu_context.hpp"
#include "support/render_readback.hpp"

namespace {

namespace r = engine::renderer;
namespace rhi = engine::rhi;

[[nodiscard]] r::CloudSettings overcast() {
    r::CloudSettings c;
    c.coverage = 0.6F;
    c.density = 40.0F;
    c.time_s = 0.0F;
    c.offset_km = {12.3F, -4.5F};
    return c;
}

// The layer the frame builds for `c` (world-anchored at its offset).
[[nodiscard]] r::CloudLayerGpu layer_of(const r::CloudSettings& c, const glm::vec3& sun) {
    return r::make_cloud_layer(c, true, glm::vec3(0.0F), r::CloudLights{.sun_to = sun});
}

// The CPU-baked noise (shared by the tests of this file).
const r::cloud_noise::Volumes& noise() {
    static const r::cloud_noise::Volumes v = r::cloud_noise::bake_cpu();
    return v;
}

// Camera-relative km offset of a shadow-map texel line point, in the frame
// the map integrates (the camera stands 0.2 km above the cloud frame's ground).
[[nodiscard]] glm::vec3 rel_km(const glm::vec3& world, const glm::vec3& camera) {
    glm::vec3 rel = (world - camera) * 0.001F;
    rel.y += 0.2F;
    return rel;
}

TEST(CloudShadowFrame, TexelsProjectToTheirOwnCentres) {
    const r::CloudShadowFrame f =
        r::CloudShadowFrame::make(glm::normalize(glm::vec3(0.4F, 0.7F, -0.3F)),
                                  glm::vec3(1234.0F, 20.0F, -876.0F));
    const r::CloudShadowProjection p = f.projection();
    for (std::uint32_t y : {0U, 100U, 511U}) {
        for (std::uint32_t x : {0U, 257U, 511U}) {
            const glm::vec3 at = f.texel_point(x, y);
            EXPECT_NEAR(at.y, f.camera.y, 1e-2F); // at the camera's height
            const glm::vec2 uv = p.uv(at);
            EXPECT_NEAR(uv.x * r::cloud_shadow_resolution, static_cast<float>(x) + 0.5F, 1e-2F);
            EXPECT_NEAR(uv.y * r::cloud_shadow_resolution, static_cast<float>(y) + 0.5F, 1e-2F);
            // Every point on the texel's line toward the sun shares it.
            const glm::vec2 up_line = p.uv(at + f.sun * 300.0F);
            EXPECT_NEAR(glm::length(up_line - uv), 0.0F, 1e-4F);
        }
    }
}

TEST(CloudShadowFrame, SnapsToTexelsAndHoldsAtLowSun) {
    const glm::vec3 sun = glm::normalize(glm::vec3(0.4F, 0.7F, -0.3F));
    const r::CloudShadowFrame a = r::CloudShadowFrame::make(sun, glm::vec3(100.0F, 0.0F, 50.0F));
    const r::CloudShadowFrame b = r::CloudShadowFrame::make(sun, glm::vec3(100.5F, 0.0F, 50.5F));
    EXPECT_EQ(a.centre, b.centre) << "less than a texel of camera motion keeps the map";
    EXPECT_FLOAT_EQ(std::fmod(std::abs(a.centre.x), a.texel_m), 0.0F);

    // Below the minimum elevation (and below the horizon) the map holds the
    // lowest sun it follows: no pop to clear sky at dusk.
    const r::CloudShadowFrame dusk = r::CloudShadowFrame::make(
        glm::normalize(glm::vec3(1.0F, 0.01F, 0.0F)), glm::vec3(0.0F));
    const r::CloudShadowFrame night = r::CloudShadowFrame::make(
        glm::normalize(glm::vec3(1.0F, -0.3F, 0.0F)), glm::vec3(0.0F));
    EXPECT_NEAR(dusk.sun.y, r::cloud_shadow_min_sun_y, 1e-6F);
    EXPECT_NEAR(glm::length(dusk.sun - night.sun), 0.0F, 1e-6F);
    // Under a full overcast a low sun is still blocked (a long path through
    // the shell), not treated as clear.
    r::CloudSettings full = overcast();
    full.coverage = 1.0F;
    const glm::vec3 ground(0.0F, 0.2F, 0.0F);
    EXPECT_LT(r::cloud_reference::shadow_transmittance(layer_of(full, dusk.sun), noise(), ground,
                                                       dusk.sun, r::cloud_shadow_steps),
              0.2F);
    EXPECT_EQ(r::cloud_reference::shadow_transmittance(
                  layer_of(r::CloudSettings{.coverage = 0.0F}, dusk.sun), noise(), ground,
                  dusk.sun, r::cloud_shadow_steps),
              1.0F);
}

TEST(GpuCloudShadow, MapMatchesTheCpuReference) {
    QUAT_REQUIRE_GPU(g);
    auto pass = r::CloudShadowPass::create(g->device, g->allocator, g->cache, g->shader_dir);
    ASSERT_OK(pass);
    auto gpu_noise = r::CloudNoisePass::create(g->device, g->allocator, g->cache, g->shader_dir);
    ASSERT_OK(gpu_noise);
    const glm::vec3 sun = glm::normalize(glm::vec3(-0.35F, 0.6F, 0.45F));
    const glm::vec3 camera(-1500.0F, 35.0F, 2200.0F);
    const r::CloudSettings clouds = overcast();
    const r::CloudLayerGpu layer = layer_of(clouds, sun);

    rhi::TransientImagePool pool(g->device, g->allocator);
    rhi::RenderGraph graph(pool);
    auto map = pass->add_to_graph(graph, sun, camera, layer, *gpu_noise);
    ASSERT_OK(map);
    constexpr VkExtent2D extent{r::cloud_shadow_resolution, r::cloud_shadow_resolution};
    auto px = quat::test::render_to_host(*g, graph, map->texture, extent, 2);
    ASSERT_OK(px);

    const r::CloudShadowFrame f = r::CloudShadowFrame::make(sun, camera);
    std::vector<float> errors;
    float lo = 1.0F;
    float hi = 0.0F;
    for (std::uint32_t y = 3; y < extent.height; y += 16) {
        for (std::uint32_t x = 5; x < extent.width; x += 16) {
            std::uint16_t h = 0;
            std::memcpy(&h, px->at(x, y), sizeof(h));
            const float gpu = glm::unpackHalf1x16(h);
            const float ref = r::cloud_reference::shadow_transmittance(
                layer, noise(), rel_km(f.texel_point(x, y), camera), f.sun,
                r::cloud_shadow_steps);
            errors.push_back(std::abs(gpu - ref));
            lo = std::min(lo, ref);
            hi = std::max(hi, ref);
        }
    }
    // The density stretches the 8-bit noise (shape remap: gain ~4.3, then
    // the coverage remap), so texture filtering's sub-texel precision —
    // implementation-defined, 8 bits on lavapipe — shows in a few edge
    // texels; the mean stays small.
    std::sort(errors.begin(), errors.end());
    EXPECT_LT(errors[errors.size() * 99 / 100], 0.08F) << "99th percentile";
    EXPECT_LT(errors[errors.size() / 2], 0.002F) << "median";
    double mean = 0.0;
    for (const float e : errors) mean += e;
    EXPECT_LT(mean / static_cast<double>(errors.size()), 0.008);
    EXPECT_LT(lo, 0.5F) << "the map has shadowed texels";
    EXPECT_GT(hi, 0.9F) << "and clear ones";
}

} // namespace
