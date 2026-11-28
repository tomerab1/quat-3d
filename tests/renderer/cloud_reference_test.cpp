// The cloud model's CPU reference (R13.4, cloud_reference.hpp mirrors
// lib/clouds.slang): the step integration is exact for a constant medium
// (Hillaire 2015's energy-conserving form), the phase functions are
// normalised, Wrenninge's octaves never add energy beyond their weights, a
// lit slab matches its closed form, and the layer the frame builds follows
// the weather (anchoring, downwind drift, type, lights).

#include <cmath>
#include <numbers>

#include <glm/glm.hpp>
#include <gtest/gtest.h>

#include "engine/renderer/cloud_reference.hpp"

namespace {

namespace r = engine::renderer;
namespace cr = engine::renderer::cloud_reference;

[[nodiscard]] r::CloudLayerGpu default_layer() {
    return r::make_cloud_layer(r::CloudSettings{}, false, glm::vec3(0.0F), r::CloudLights{});
}

TEST(CloudReference, StepIntegrationIsExactForAConstantMedium) {
    const glm::vec3 s(0.3F, 0.5F, 0.7F);
    for (const float sigma : {0.01F, 1.0F, 25.0F}) {
        const float length = 2.0F;
        const glm::vec3 exact = s * (1.0F - std::exp(-sigma * length)) / sigma;
        for (const int steps : {1, 7, 64}) {
            float trans = 1.0F;
            glm::vec3 sum(0.0F);
            const float dt = length / static_cast<float>(steps);
            for (int i = 0; i < steps; ++i) sum += cr::integrate_step(s, sigma, dt, trans);
            // Float rounding only: 1 - exp(-x) cancels for small x (~1e-4
            // relative at sigma dt = 3e-4), identically on the GPU.
            for (int k = 0; k < 3; ++k) EXPECT_NEAR(sum[k], exact[k], 3e-4F * exact[k]);
            EXPECT_NEAR(trans, std::exp(-sigma * length), 1e-6F);
        }
    }
    // A naive Riemann sum (S dt per step) overshoots in dense media: the
    // reason for the closed form (Hillaire 2015, fig. 13).
    float trans = 1.0F;
    const glm::vec3 one(1.0F);
    const glm::vec3 exact = cr::integrate_step(one, 25.0F, 0.5F, trans);
    EXPECT_LT(exact.x, 0.5F);
}

TEST(CloudReference, PhaseFunctionsAreNormalised) {
    const r::CloudLayerGpu l = default_layer();
    for (const float g : {-0.5F, 0.0F, 0.3F, 0.8F}) {
        double hg = 0.0;
        double dual = 0.0;
        constexpr int n = 20000;
        for (int i = 0; i < n; ++i) {
            const double mu = -1.0 + (static_cast<double>(i) + 0.5) * 2.0 / n;
            hg += cr::hg_phase(static_cast<float>(mu), g) * (2.0 / n);
            dual += cr::cloud_phase(l, static_cast<float>(mu), std::max(g, 0.0F) + 0.1F) *
                    (2.0 / n);
        }
        // Integral over the sphere = 2 pi * integral over mu.
        EXPECT_NEAR(hg * 2.0 * std::numbers::pi, 1.0, 2e-3) << "g " << g;
        EXPECT_NEAR(dual * 2.0 * std::numbers::pi, 1.0, 2e-3) << "g " << g;
    }
}

TEST(CloudReference, OctavesConserveEnergy) {
    const r::CloudLayerGpu l = default_layer();
    ASSERT_LE(l.octaves.x, l.octaves.y) << "a <= b (Wrenninge 2013)";
    const glm::vec3 e(1.0F);
    const float sigma_s = 2.0F;
    float weight = 0.0F;
    float a = 1.0F;
    for (int i = 0; i < static_cast<int>(l.octaves.w); ++i, a *= l.octaves.x) weight += a;
    float previous = 1e9F;
    for (const float tau : {0.0F, 0.5F, 2.0F, 8.0F, 30.0F}) {
        // Over the sphere of view directions each octave's phase integrates
        // to 1: the scattered energy is sigma_s E sum a^i exp(-b^i tau).
        double total = 0.0;
        constexpr int n = 8000;
        for (int i = 0; i < n; ++i) {
            const double mu = -1.0 + (static_cast<double>(i) + 0.5) * 2.0 / n;
            total += cr::light_scattering(l, e, sigma_s, tau, static_cast<float>(mu)).x *
                     (2.0 / n) * 2.0 * std::numbers::pi;
        }
        EXPECT_LE(total, sigma_s * weight * 1.002) << "never more than the octave weights";
        EXPECT_GE(total, sigma_s * std::exp(-tau) * 0.998) << "never less than single scattering";
        EXPECT_LT(total, previous) << "deeper inside, less light";
        previous = static_cast<float>(total);
    }
}

// A homogeneous slab under an overhead light, seen from straight above:
// single scattering reflected toward the light's direction has the closed
// form E sigma_s p (1 - exp(-2 sigma H)) / (2 sigma).
TEST(CloudReference, LitSlabMatchesItsClosedForm) {
    r::CloudLayerGpu l = default_layer();
    l.octaves.w = 1.0F; // single scattering
    const float sigma = 3.0F;
    const float sigma_s = sigma * l.medium.y;
    const float height = 1.2F;
    const glm::vec3 e(1.0F);
    const float cos_theta = -1.0F; // light from above, viewer above: backscatter
    constexpr int steps = 512;
    const float dt = height / steps;
    float trans = 1.0F;
    glm::vec3 lum(0.0F);
    for (int i = 0; i < steps; ++i) {
        const float depth = (static_cast<float>(i) + 0.5F) * dt; // below the top
        const glm::vec3 s = cr::light_scattering(l, e, sigma_s, sigma * depth, cos_theta);
        lum += cr::integrate_step(s, sigma, dt, trans);
    }
    const float p = cr::cloud_phase(l, cos_theta, 1.0F);
    const float exact = sigma_s * p * (1.0F - std::exp(-2.0F * sigma * height)) / (2.0F * sigma);
    EXPECT_NEAR(lum.x, exact, 0.005F * exact);
}

TEST(CloudLayer, FollowsSettingsWeatherAndLights) {
    r::CloudSettings c;
    c.coverage = 0.5F;
    c.wind_speed = 10.0F;
    c.wind_dir_deg = 0.0F; // toward +X
    c.time_s = 100.0F;
    const glm::vec3 camera(2000.0F, 50.0F, -1000.0F);
    const r::CloudLayerGpu l = r::make_cloud_layer(c, false, camera, r::CloudLights{});
    // Field coordinate = world - drift: anchor = camera km - drift (1 km).
    EXPECT_NEAR(l.offset.x, 2.0F - 1.0F, 1e-5F);
    EXPECT_NEAR(l.offset.y, -1.0F, 1e-5F);
    // The detail field drifts further downwind than the base.
    EXPECT_LT(l.offset.z, 0.0F);

    // With weather the frame's offset is used as is.
    r::CloudSettings w = c;
    w.offset_km = {7.0F, -3.0F};
    const r::CloudLayerGpu lw = r::make_cloud_layer(w, true, camera, r::CloudLights{});
    EXPECT_EQ(lw.offset.x, 7.0F);
    EXPECT_EQ(lw.offset.y, -3.0F);

    // Disabled: no coverage, no cirrus.
    r::CloudSettings off = c;
    off.enabled = false;
    const r::CloudLayerGpu lo = r::make_cloud_layer(off, false, camera, r::CloudLights{});
    EXPECT_EQ(lo.shell.z, 0.0F);
    EXPECT_EQ(lo.cirrus.x, 0.0F);

    // A closing sky turns toward layered cloud.
    r::CloudSettings closed = c;
    closed.coverage = 1.0F;
    closed.type = 1.0F;
    EXPECT_LT(r::make_cloud_layer(closed, false, camera, r::CloudLights{}).shell.w, 0.5F);

    // The moon's light comes through unchanged (relative irradiance).
    r::CloudLights lights;
    lights.moon_to = glm::normalize(glm::vec3(0.0F, 1.0F, 1.0F));
    lights.moon_irradiance = 2.0e-6F;
    const r::CloudLayerGpu lm = r::make_cloud_layer(c, false, camera, lights);
    EXPECT_NEAR(lm.moon.w, 2.0e-6F, 1e-12F);
    EXPECT_NEAR(glm::length(glm::vec3(lm.moon)), 1.0F, 1e-5F);
}

TEST(CloudLayer, HeightProfilesFollowTheType) {
    // Stratus hug the base; cumulus reach the top of the shell.
    EXPECT_GT(cr::height_profile(0.1F, 0.0F), 0.5F);
    EXPECT_EQ(cr::height_profile(0.5F, 0.0F), 0.0F);
    EXPECT_GT(cr::height_profile(0.5F, 1.0F), 0.9F);
    EXPECT_EQ(cr::height_profile(0.0F, 1.0F), 0.0F);
    EXPECT_EQ(cr::height_profile(1.0F, 1.0F), 0.0F);
}

} // namespace
