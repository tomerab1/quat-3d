// Physically based atmosphere LUTs, read back after ensure_skyview with a high
// sun: the transmittance LUT reddens long horizon paths relative to the
// zenith, and the sky-view LUT is blue above the horizon and ground-coloured
// below.

#include <cstddef>
#include <cstdint>
#include <expected>
#include <vector>

#include <glm/glm.hpp>
#include <gtest/gtest.h>
#include <vulkan/vulkan.h>

#include "engine/renderer/atmosphere_pass.hpp"
#include "support/expect_ok.hpp"
#include "support/gpu_context.hpp"
#include "support/render_readback.hpp"

namespace {

namespace r = engine::renderer;
using engine::core::Error;

constexpr VkExtent2D k_trans = r::AtmospherePass::transmittance_extent;
constexpr VkExtent2D k_sky = r::AtmospherePass::skyview_extent;

struct Luts {
    std::vector<glm::vec4> transmittance; // y = 0 is ground level; x = 0 the zenith ray
    std::vector<glm::vec4> skyview;
};

std::expected<Luts, Error> compute_luts(quat::test::GpuContext& g) {
    auto pass = r::AtmospherePass::create(g.device, g.allocator, g.cache, g.shader_dir);
    if (!pass) return std::unexpected(pass.error());
    if (auto s = pass->ensure_skyview(glm::normalize(glm::vec3(0.3F, 1.0F, 0.2F))); !s) {
        return std::unexpected(s.error());
    }
    Luts out;
    auto trans =
        quat::test::read_rgba16f(g, pass->transmittance_image(), 0, 0, k_trans.width, k_trans.height);
    if (!trans) return std::unexpected(trans.error());
    out.transmittance = std::move(*trans);
    auto sky = quat::test::read_rgba16f(g, pass->skyview_image(), 0, 0, k_sky.width, k_sky.height);
    if (!sky) return std::unexpected(sky.error());
    out.skyview = std::move(*sky);
    return out;
}

const std::expected<Luts, Error>& luts(quat::test::GpuContext& g) {
    static const std::expected<Luts, Error> out = compute_luts(g);
    return out;
}

[[nodiscard]] glm::vec4 ground_level(const Luts& l, std::uint32_t x) { return l.transmittance[x]; }

// Mean of a sky-view row, so individual (white-ish) cloud texels cannot skew it.
[[nodiscard]] glm::vec4 row_mean(const Luts& l, std::uint32_t y) {
    glm::vec4 sum(0.0F);
    for (std::uint32_t x = 0; x < k_sky.width; ++x) {
        sum += l.skyview[static_cast<std::size_t>(y) * k_sky.width + x];
    }
    return sum / static_cast<float>(k_sky.width);
}

} // namespace

TEST(GpuAtmosphere, ZenithTransmittanceIsInRange) {
    QUAT_REQUIRE_GPU(g);
    const auto& l = luts(*g);
    ASSERT_OK(l);
    const float zenith_b = ground_level(*l, 0).b;
    EXPECT_GT(zenith_b, 0.2F);
    EXPECT_LE(zenith_b, 1.0F);
}

// The zenith is clearer than the horizon, and the horizon ray is reddened:
// blue scatters out over the long path.
TEST(GpuAtmosphere, HorizonRayIsReddened) {
    QUAT_REQUIRE_GPU(g);
    const auto& l = luts(*g);
    ASSERT_OK(l);
    const glm::vec4 zenith = ground_level(*l, 0);
    const glm::vec4 horizon = ground_level(*l, k_trans.width - 1);
    EXPECT_GT(zenith.b, horizon.b);
    EXPECT_GT(horizon.r, horizon.b);
}

TEST(GpuAtmosphere, SkyAboveTheHorizonIsBlue) {
    QUAT_REQUIRE_GPU(g);
    const auto& l = luts(*g);
    ASSERT_OK(l);
    const glm::vec4 up = row_mean(*l, k_sky.height / 8);
    EXPECT_GT(up.b, 1e-4F);
    EXPECT_GT(up.b, up.r);
}

// Below the horizon is the lit ground, not more sky: under a high sun it is
// far less blue than the sky. (Not dimmer: sunlit ground of albedo 0.24 is
// as bright as a clear zenith sky, ~7000 cd/m²; R22.2.1 fixed the sky-view
// mapping that had every ground ray read the nadir row.)
TEST(GpuAtmosphere, BelowTheHorizonIsTheGroundNotTheSky) {
    QUAT_REQUIRE_GPU(g);
    const auto& l = luts(*g);
    ASSERT_OK(l);
    const glm::vec4 ground = row_mean(*l, k_sky.height - 2);
    const glm::vec4 sky = row_mean(*l, k_sky.height / 8);
    ASSERT_GT(ground.r, 0.0F);
    ASSERT_GT(sky.r, 0.0F);
    EXPECT_LT(ground.b / ground.r, 0.6F * sky.b / sky.r);
}
