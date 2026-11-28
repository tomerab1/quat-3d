// Froxel volumetric fog (R13.3) on the GPU against closed forms: a uniform
// medium's transmittance and in-scattering at an opaque surface match
// L = B e^{-sigma d} + albedo A (1 - e^{-sigma d}) (the energy-conserving
// integration is exact for a constant medium, whatever the slicing), the
// analytic height fog continues the froxel grid without a seam, a local fog
// volume only affects the rays that cross it, and the result is stable and
// deterministic frame to frame.

#include <cmath>
#include <cstdint>

#include <glm/glm.hpp>
#include <gtest/gtest.h>

#include "engine/renderer/fog_pass.hpp"
#include "support/expect_ok.hpp"
#include "support/gpu_context.hpp"
#include "support/volumetrics_harness.hpp"

namespace {

namespace r = engine::renderer;
using quat::test::HostImage;
using quat::test::VolumetricsHarness;
using quat::test::VolumetricsScene;

constexpr glm::vec3 k_background{0.2F, 0.5F, 1.0F};
constexpr glm::vec3 k_ambient{0.6F, 0.6F, 0.6F};

// A camera looking straight down -Z at a wall (constant depth buffer).
[[nodiscard]] VolumetricsScene fog_scene(float density, float depth) {
    VolumetricsScene s;
    s.atmosphere = false;
    s.fog = true;
    s.camera = {0.0F, 2.0F, 0.0F};
    s.target = {0.0F, 2.0F, -10.0F};
    s.scene_depth = depth;
    s.background = glm::vec4(k_background, 1.0F);
    s.media.extinction = density;
    s.media.height_falloff = 0.0F; // uniform
    s.media.base_height_m = 1000.0F;
    s.media.albedo = glm::vec3(0.9F);
    s.media.ambient = k_ambient;
    s.media.max_distance_m = 400.0F;
    s.media.sun_radiance = glm::vec3(0.0F);
    return s;
}

// Distance from the camera to the wall at the image centre for depth `z`
// (the harness's projection: 70 deg, near 0.1, far 5000).
[[nodiscard]] float wall_distance(float z) {
    const float n = 0.1F;
    const float f = 5000.0F;
    return n * f / (f - z * (f - n)); // inverse of the [0, 1] perspective depth
}

[[nodiscard]] glm::vec3 centre(const HostImage& img) {
    glm::vec3 sum(0.0F);
    const glm::uvec2 c = img.centre();
    for (std::uint32_t y = c.y - 2; y <= c.y + 2; ++y) {
        for (std::uint32_t x = c.x - 2; x <= c.x + 2; ++x) sum += glm::vec3(img.rgba16f(x, y));
    }
    return sum / 25.0F;
}

[[nodiscard]] glm::vec3 expected(float sigma, float d, float albedo) {
    const float t = std::exp(-sigma * d);
    return k_background * t + albedo * k_ambient * (1.0F - t);
}

[[nodiscard]] float depth_at(float d) {
    const float n = 0.1F;
    const float f = 5000.0F;
    return (f - n * f / d) / (f - n);
}

TEST(FogSlices, DepthMappingRoundTrips) {
    for (const float far_m : {100.0F, 1000.0F, 8000.0F}) {
        EXPECT_NEAR(r::fog_slice_depth(0.0F, far_m), 0.0F, 1e-6F);
        EXPECT_NEAR(r::fog_slice_depth(1.0F, far_m), far_m, far_m * 1e-5F);
        for (const float d : {0.5F, 7.0F, 60.0F, far_m * 0.9F}) {
            EXPECT_NEAR(r::fog_slice_depth(r::fog_slice_w(d, far_m), far_m), d, d * 1e-4F);
        }
    }
}

TEST(FogVolumes, UnitVolumeTransform) {
    const glm::mat4 world = glm::translate(glm::mat4(1.0F), glm::vec3(10.0F, 0.0F, -5.0F)) *
                            glm::scale(glm::mat4(1.0F), glm::vec3(4.0F, 2.0F, 8.0F));
    const r::FogVolumeGpu v = r::make_fog_volume(world, false, 0.05F, glm::vec3(0.8F), 0.2F, 0.0F);
    // The world corner (14, 2, 3) maps to the unit corner (1, 1, 1).
    const glm::vec4 p(14.0F, 2.0F, 3.0F, 1.0F);
    EXPECT_NEAR(glm::dot(v.row0, p), 1.0F, 1e-5F);
    EXPECT_NEAR(glm::dot(v.row1, p), 1.0F, 1e-5F);
    EXPECT_NEAR(glm::dot(v.row2, p), 1.0F, 1e-5F);
    EXPECT_EQ(v.albedo_density.w, 0.05F);
}

TEST(GpuVolumetricFog, UniformMediumMatchesClosedForm) {
    QUAT_REQUIRE_GPU(g);
    for (const float sigma : {0.002F, 0.01F, 0.05F}) {
        for (const float d : {40.0F, 150.0F, 350.0F}) {
            auto h = VolumetricsHarness::create(*g);
            ASSERT_OK(h);
            const float z = depth_at(d);
            ASSERT_NEAR(wall_distance(z), d, d * 1e-3F);
            auto out = (*h)->render(fog_scene(sigma, z), 0, 0.0F);
            ASSERT_OK(out);
            const glm::vec3 got = centre(out->composite);
            const glm::vec3 want = expected(sigma, d, 0.9F);
            for (int k = 0; k < 3; ++k) {
                EXPECT_NEAR(got[k], want[k], 0.02F * want[k] + 2e-3F)
                    << "sigma " << sigma << " d " << d << " channel " << k;
            }
        }
    }
}

TEST(GpuVolumetricFog, AnalyticFogContinuesTheGrid) {
    QUAT_REQUIRE_GPU(g);
    auto h = VolumetricsHarness::create(*g);
    ASSERT_OK(h);
    // Just inside and just beyond the grid's 400 m range: no seam, and the
    // far wall matches the closed form over its whole distance.
    auto inside = (*h)->render(fog_scene(0.004F, depth_at(390.0F)), 0, 0.0F);
    ASSERT_OK(inside);
    (*h)->reset_history();
    auto beyond = (*h)->render(fog_scene(0.004F, depth_at(410.0F)), 0, 0.0F);
    ASSERT_OK(beyond);
    (*h)->reset_history();
    auto far_wall = (*h)->render(fog_scene(0.004F, depth_at(900.0F)), 0, 0.0F);
    ASSERT_OK(far_wall);
    const glm::vec3 a = centre(inside->composite);
    const glm::vec3 b = centre(beyond->composite);
    EXPECT_LT(glm::length(a - b), 0.03F * glm::length(a)) << "no seam at the grid's range";
    const glm::vec3 want = expected(0.004F, 900.0F, 0.9F);
    const glm::vec3 got = centre(far_wall->composite);
    for (int k = 0; k < 3; ++k) EXPECT_NEAR(got[k], want[k], 0.03F * want[k] + 2e-3F);
}

TEST(GpuVolumetricFog, LocalVolumeOnlyTouchesItsRays) {
    QUAT_REQUIRE_GPU(g);
    auto h = VolumetricsHarness::create(*g);
    ASSERT_OK(h);
    VolumetricsScene s = fog_scene(0.0F, depth_at(100.0F));
    // A 4 m cube of dense fog 30 m ahead, left of centre.
    const glm::mat4 world = glm::translate(glm::mat4(1.0F), glm::vec3(-6.0F, 2.0F, -30.0F)) *
                            glm::scale(glm::mat4(1.0F), glm::vec3(2.0F));
    s.volumes.push_back(r::make_fog_volume(world, false, 0.5F, glm::vec3(0.9F), 0.0F, 0.0F));
    HostImage last;
    for (std::uint64_t f = 0; f < 12; ++f) {
        auto out = (*h)->render(s, f, 0.0F);
        ASSERT_OK(out);
        last = out->composite;
    }
    quat::test::dump_ppm(last, "fog_volume", 1.0F);
    // Project the cube's centre (harness camera: 70 deg vertical fov).
    const float aspect = static_cast<float>(s.extent.width) / static_cast<float>(s.extent.height);
    const float tan_half = std::tan(glm::radians(35.0F));
    const float ndc_x = (-6.0F / 30.0F) / (tan_half * aspect);
    const auto x = static_cast<std::uint32_t>((ndc_x * 0.5F + 0.5F) * static_cast<float>(s.extent.width));
    const std::uint32_t y = s.extent.height / 2;
    const glm::vec3 in_volume(last.rgba16f(x, y));
    const glm::vec3 clear(last.rgba16f(s.extent.width - 10, y));
    EXPECT_NEAR(clear.b, k_background.b, 0.01F) << "rays that miss it see the background";
    // 4 m at 0.5 /m: optical depth 2, the volume dominates.
    EXPECT_LT(in_volume.b, 0.5F * k_background.b + 0.9F * k_ambient.b);
    EXPECT_GT(in_volume.r, k_background.r + 0.2F);
}

TEST(GpuVolumetricFog, StableAndDeterministic) {
    QUAT_REQUIRE_GPU(g);
    VolumetricsScene s = fog_scene(0.01F, depth_at(200.0F));
    s.media.sun_radiance = glm::vec3(3.0F);
    s.media.sun_to = glm::normalize(glm::vec3(0.3F, 0.4F, -1.0F));
    std::vector<std::uint8_t> runs[2];
    for (int run = 0; run < 2; ++run) {
        auto h = VolumetricsHarness::create(*g);
        ASSERT_OK(h);
        HostImage prev;
        HostImage last;
        for (std::uint64_t f = 0; f < 10; ++f) {
            auto out = (*h)->render(s, f, 0.0F);
            ASSERT_OK(out);
            prev = last;
            last = out->composite;
        }
        const glm::vec3 a = centre(prev);
        const glm::vec3 b = centre(last);
        EXPECT_LT(glm::length(a - b), 0.01F * glm::length(b)) << "a static view settles";
        runs[run] = last.bytes;
    }
    EXPECT_EQ(runs[0], runs[1]);
}

} // namespace
