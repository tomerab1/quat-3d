// The planet surface below the horizon (R22.2.1, issues #98 and #13): a
// Lambertian ground lit by the transmitted Sun / Moon and the sky irradiance
// LUT, seen through the sky-view LUT's aerial perspective.
//   - It meets the sky without a step: across sun elevations -6..90 deg the
//     luminance just above and just below the horizon differ by < 1.5x.
//   - At a 2.5 deg sun it is not the saturated rust of #98: skylight keeps
//     it bluish-neutral.
//   - Under a moon alone, the ground straight down has the luminance of an
//     albedo-0.24 surface under the moon's (transmitted) illuminance.
//   - The LUT's per-update scale keeps a moonlit night inside RGBA16F.

#include <cmath>
#include <cstddef>
#include <cstdint>
#include <expected>
#include <vector>

#include <glm/glm.hpp>
#include <gtest/gtest.h>
#include <vulkan/vulkan.h>

#include "engine/renderer/atmosphere_pass.hpp"
#include "engine/renderer/photometry.hpp"
#include "support/expect_ok.hpp"
#include "support/gpu_context.hpp"
#include "support/render_readback.hpp"

namespace {

namespace r = engine::renderer;
using engine::core::Error;

constexpr VkExtent2D k_sky = r::AtmospherePass::skyview_extent;
// lib/atmosphere.slang ATMOS_GROUND_ALBEDO.
const glm::vec3 k_albedo{0.24F, 0.25F, 0.21F};

[[nodiscard]] glm::vec3 toward(float elevation_deg, float azimuth_deg) {
    const float e = glm::radians(elevation_deg);
    const float az = glm::radians(azimuth_deg);
    return {std::cos(e) * std::sin(az), std::sin(e), -std::cos(e) * std::cos(az)};
}

// The decoded sky-view LUT (luminance per unit extra-atmospheric sun).
[[nodiscard]] std::expected<std::vector<glm::vec3>, Error>
skyview(quat::test::GpuContext& g, r::AtmospherePass& pass, const glm::vec3& sun,
        const r::SkyLights& lights) {
    if (auto s = pass.ensure_skyview(sun, lights); !s) return std::unexpected(s.error());
    auto raw = quat::test::read_rgba16f(g, pass.skyview_image(), 0, 0, k_sky.width, k_sky.height);
    if (!raw) return std::unexpected(raw.error());
    std::vector<glm::vec3> out;
    out.reserve(raw->size());
    for (const glm::vec4& t : *raw) out.push_back(glm::vec3(t) * std::exp2(t.a));
    return out;
}

[[nodiscard]] float luminance(const glm::vec3& c) {
    return 0.2126F * c.r + 0.7152F * c.g + 0.0722F * c.b;
}

[[nodiscard]] glm::vec3 texel(const std::vector<glm::vec3>& lut, std::uint32_t x, std::uint32_t y) {
    return lut[static_cast<std::size_t>(y) * k_sky.width + x];
}

} // namespace

TEST(GpuAtmosphereGround, MeetsTheSkyWithoutAStepAtEverySunElevation) {
    QUAT_REQUIRE_GPU(g);
    auto pass = r::AtmospherePass::create(g->device, g->allocator, g->cache, g->shader_dir);
    ASSERT_OK(pass);
    r::SkyLights lights;
    lights.night_irradiance = r::photometry::k_night_sky_floor_lux / r::photometry::k_sun_reference_lux;
    // Rows just above (v < 0.5) and just below (v > 0.5) the horizon.
    const std::uint32_t above = k_sky.height / 2 - 1;
    const std::uint32_t below = k_sky.height / 2;
    for (const float elevation : {-6.0F, -3.0F, 0.0F, 2.5F, 5.0F, 10.0F, 30.0F, 60.0F, 90.0F}) {
        auto lut = skyview(*g, *pass, toward(elevation, 0.0F), lights);
        ASSERT_OK(lut) << elevation;
        for (std::uint32_t x = 0; x < k_sky.width; x += 8) {
            const float sky = luminance(texel(*lut, x, above));
            const float ground = luminance(texel(*lut, x, below));
            ASSERT_GT(sky, 0.0F) << elevation << " deg, column " << x;
            ASSERT_GT(ground, 0.0F) << elevation << " deg, column " << x;
            const float ratio = std::max(sky, ground) / std::min(sky, ground);
            EXPECT_LT(ratio, 1.5F) << "sun " << elevation << " deg, column " << x;
        }
    }
}

// #98: at a 2.5 deg sun the old ground was RGB ~(110, 40, 10): reddened
// direct sun only. Lit by the (blue) sky as well, the nearby ground keeps
// a neutral-to-cool balance.
TEST(GpuAtmosphereGround, LowSunGroundIsNotRust) {
    QUAT_REQUIRE_GPU(g);
    auto pass = r::AtmospherePass::create(g->device, g->allocator, g->cache, g->shader_dir);
    ASSERT_OK(pass);
    auto lut = skyview(*g, *pass, toward(2.5F, 0.0F), {});
    ASSERT_OK(lut);
    // Straight down (last row): the ground 200 m below, little air between.
    glm::vec3 down(0.0F);
    for (std::uint32_t x = 0; x < k_sky.width; ++x) down += texel(*lut, x, k_sky.height - 1);
    EXPECT_GT(down.b / down.r, 0.5F) << "ground " << down.r << " " << down.g << " " << down.b;
}

// Moon at 45 deg, sun far below: the ground straight down is the albedo
// over pi times the moon's transmitted irradiance plus its skylight and the
// night floor. The direct term dominates; allow 35 % for the skylight and
// the little air between.
TEST(GpuAtmosphereGround, MoonlitGroundHasTheMoonsIlluminance) {
    QUAT_REQUIRE_GPU(g);
    auto pass = r::AtmospherePass::create(g->device, g->allocator, g->cache, g->shader_dir);
    ASSERT_OK(pass);
    namespace ph = r::photometry;
    constexpr float k_moon_lux = 0.27F; // a full moon above the atmosphere
    r::SkyLights lights;
    lights.moon_direction = toward(45.0F, 120.0F);
    lights.moon_irradiance = k_moon_lux / ph::k_sun_reference_lux;
    lights.night_irradiance = ph::k_night_sky_floor_lux / ph::k_sun_reference_lux;
    auto lut = skyview(*g, *pass, toward(-40.0F, 0.0F), lights);
    ASSERT_OK(lut);
    glm::vec3 down(0.0F);
    for (std::uint32_t x = 0; x < k_sky.width; ++x) down += texel(*lut, x, k_sky.height - 1);
    down /= static_cast<float>(k_sky.width);
    const float nits = luminance(down) * ph::k_sun_reference_lux; // per unit sun -> cd/m²
    // Clear-sky transmittance at 45 deg is ~0.8 (luminance-weighted).
    const float direct = k_moon_lux * 0.8F * std::sin(glm::radians(45.0F));
    const float expected = luminance(k_albedo) / 3.14159265F * (direct + ph::k_night_sky_floor_lux);
    EXPECT_NEAR(nits, expected, expected * 0.35F);
}
