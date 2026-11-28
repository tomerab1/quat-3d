// Image-based lighting maps, read back texel by texel: the blocking IblBaker's
// BRDF LUT, irradiance and prefiltered cubes, and the incrementally refreshed
// DynamicIbl probe responding to a sun move.

#include <array>
#include <cstddef>
#include <cstdint>
#include <expected>
#include <vector>

#include <glm/glm.hpp>
#include <gtest/gtest.h>
#include <vulkan/vulkan.h>

#include "engine/renderer/atmosphere_pass.hpp"
#include "engine/renderer/cloud_noise_pass.hpp"
#include "engine/renderer/cloud_reference.hpp"
#include "engine/renderer/dynamic_ibl.hpp"
#include "engine/renderer/ibl_pass.hpp"
#include "engine/rhi/render_graph.hpp"
#include "support/expect_ok.hpp"
#include "support/gpu_context.hpp"
#include "support/gpu_submit.hpp"
#include "support/render_readback.hpp"

namespace {

namespace r = engine::renderer;
namespace rhi = engine::rhi;
using engine::core::Error;

constexpr std::uint32_t k_up_face = 2; // +Y cube face

[[nodiscard]] glm::vec4 face_centre(const std::vector<glm::vec4>& texels, std::uint32_t size) {
    return texels[static_cast<std::size_t>(size / 2) * size + size / 2];
}

// The baked maps read back once for the suite.
struct BakedMaps {
    std::vector<glm::vec4> brdf_lut;
    std::vector<glm::vec4> irradiance_up;  // +Y face
    std::vector<glm::vec4> prefiltered_up; // +Y face, mip 0 (mirror)
};

std::expected<BakedMaps, Error> bake_and_read(quat::test::GpuContext& g) {
    auto baker = r::IblBaker::create(g.device, g.allocator, g.cache, g.shader_dir);
    if (!baker) return std::unexpected(baker.error());
    auto maps = baker->bake(glm::normalize(glm::vec3(0.3F, 1.0F, 0.3F)));
    if (!maps) return std::unexpected(maps.error());
    BakedMaps out;
    auto lut = quat::test::read_rgba16f(g, maps->brdf_lut.handle(), 0, 0, r::ibl_brdf_lut_size,
                                        r::ibl_brdf_lut_size);
    if (!lut) return std::unexpected(lut.error());
    out.brdf_lut = std::move(*lut);
    auto irr = quat::test::read_rgba16f(g, maps->irradiance.handle(), 0, k_up_face,
                                        r::ibl_irradiance_size, r::ibl_irradiance_size);
    if (!irr) return std::unexpected(irr.error());
    out.irradiance_up = std::move(*irr);
    auto pre = quat::test::read_rgba16f(g, maps->prefiltered.handle(), 0, k_up_face,
                                        r::ibl_prefilter_size, r::ibl_prefilter_size);
    if (!pre) return std::unexpected(pre.error());
    out.prefiltered_up = std::move(*pre);
    return out;
}

const std::expected<BakedMaps, Error>& baked(quat::test::GpuContext& g) {
    static const std::expected<BakedMaps, Error> maps = bake_and_read(g);
    return maps;
}

// Points a DynamicIbl cycle at the atmosphere LUT sky for `sun_to`.
std::expected<r::IblCycleParams, Error> lut_sky(r::AtmospherePass& atmos, const glm::vec3& sun_to) {
    r::IblCycleParams p{};
    p.sun_to = glm::normalize(sun_to);
    p.clouds.enabled = false; // keep the zenith readback deterministic
    if (auto s = atmos.ensure_skyview(p.sun_to); !s) return std::unexpected(s.error());
    p.skyview = atmos.skyview_view();
    p.transmittance = atmos.transmittance_view();
    return p;
}

[[nodiscard]] std::expected<glm::vec3, Error> front_zenith(quat::test::GpuContext& g,
                                                           const r::DynamicIbl& probe) {
    auto irr = quat::test::read_rgba16f(g, probe.front_irradiance_image(), 0, k_up_face,
                                        r::ibl_irradiance_size, r::ibl_irradiance_size);
    if (!irr) return std::unexpected(irr.error());
    return glm::vec3(face_centre(*irr, r::ibl_irradiance_size));
}

// The dynamic probe through the atmosphere LUT path — its transmittance makes
// a near-horizon sun dim the whole sky strongly, an unambiguous signal (the
// procedural fallback's zenith barely depends on the sun): zenith irradiance
// at noon after the initial blocking cycle, then after one full incremental
// cycle with a dusk sun, run one step per graph as at runtime.
struct ProbeRun {
    glm::vec3 noon_zenith{0.0F};
    glm::vec3 dusk_zenith{0.0F};
    bool      flipped = false; // the front set changed when the cycle completed
};

std::expected<ProbeRun, Error> noon_then_dusk(quat::test::GpuContext& g) {
    auto atmos = r::AtmospherePass::create(g.device, g.allocator, g.cache, g.shader_dir);
    if (!atmos) return std::unexpected(atmos.error());
    auto noon = lut_sky(*atmos, glm::vec3(0.3F, 1.0F, 0.3F));
    if (!noon) return std::unexpected(noon.error());
    auto probe = r::DynamicIbl::create(g.device, g.allocator, g.cache, g.shader_dir, *noon);
    if (!probe) return std::unexpected(probe.error());

    ProbeRun run;
    auto zenith = front_zenith(g, *probe);
    if (!zenith) return std::unexpected(zenith.error());
    run.noon_zenith = *zenith;

    const r::IblViewSet before = probe->views();
    auto dusk = lut_sky(*atmos, glm::vec3(0.95F, 0.12F, 0.0F));
    if (!dusk) return std::unexpected(dusk.error());
    for (std::uint32_t i = 0; i < r::DynamicIbl::cycle_step_count(); ++i) {
        rhi::RenderGraph graph;
        if (auto s = probe->add_step_to_graph(graph, *dusk); !s) return std::unexpected(s.error());
        if (auto c = graph.compile(); !c) return std::unexpected(c.error());
        auto ran = quat::test::submit_and_wait(g.device,
                                               [&](VkCommandBuffer cmd) { graph.execute(cmd); });
        if (!ran) return std::unexpected(ran.error());
    }
    run.flipped = probe->views().irradiance != before.irradiance;
    zenith = front_zenith(g, *probe);
    if (!zenith) return std::unexpected(zenith.error());
    run.dusk_zenith = *zenith;
    return run;
}

const std::expected<ProbeRun, Error>& probe_run(quat::test::GpuContext& g) {
    static const std::expected<ProbeRun, Error> run = noon_then_dusk(g);
    return run;
}

} // namespace

// At high NdotV and low roughness the split-sum scale tends to ~1 and the
// bias to ~0.
TEST(GpuIbl, BrdfLutScaleAndBiasAreInRange) {
    QUAT_REQUIRE_GPU(g);
    const auto& maps = baked(*g);
    ASSERT_OK(maps);
    constexpr std::uint32_t n = r::ibl_brdf_lut_size;
    const glm::vec4 texel = maps->brdf_lut[static_cast<std::size_t>(4) * n + (n - 4)];
    EXPECT_GT(texel.x, 0.8F);
    EXPECT_LE(texel.x, 1.05F);
    EXPECT_GE(texel.y, -0.01F);
    EXPECT_LT(texel.y, 0.3F);
}

// Irradiance from the +Y (up) face centre: positive and blue-dominant (the
// sky zenith).
TEST(GpuIbl, ZenithIrradianceIsSkyBlue) {
    QUAT_REQUIRE_GPU(g);
    const auto& maps = baked(*g);
    ASSERT_OK(maps);
    const glm::vec4 c = face_centre(maps->irradiance_up, r::ibl_irradiance_size);
    EXPECT_GT(c.r, 0.0F);
    EXPECT_GT(c.b, 0.0F);
    EXPECT_GT(c.b, c.r);
}

// The prefiltered mip 0 (mirror) +Y face centre is a near-mirror sky sample:
// positive and b >= r (the exact texel may land on a white cloud, b == r).
TEST(GpuIbl, PrefilteredMirrorIsSkyBlue) {
    QUAT_REQUIRE_GPU(g);
    const auto& maps = baked(*g);
    ASSERT_OK(maps);
    const glm::vec4 c = face_centre(maps->prefiltered_up, r::ibl_prefilter_size);
    EXPECT_GT(c.r, 0.0F);
    EXPECT_GE(c.b, c.r);
}

// The initial blocking cycle lands sky-blue zenith irradiance in the front set.
TEST(GpuDynamicIbl, InitialCycleIsSkyBlue) {
    QUAT_REQUIRE_GPU(g);
    const auto& run = probe_run(*g);
    ASSERT_OK(run);
    EXPECT_GT(run->noon_zenith.b, 0.0F);
    EXPECT_GT(run->noon_zenith.b, run->noon_zenith.r);
}

TEST(GpuDynamicIbl, CompletedCycleFlipsTheProbeSets) {
    QUAT_REQUIRE_GPU(g);
    const auto& run = probe_run(*g);
    ASSERT_OK(run);
    EXPECT_TRUE(run->flipped);
}

// Dusk irradiance is finite, and the zenith dims measurably as the sun drops
// to the horizon.
TEST(GpuDynamicIbl, IrradianceFollowsTheSunDown) {
    QUAT_REQUIRE_GPU(g);
    const auto& run = probe_run(*g);
    ASSERT_OK(run);
    EXPECT_GE(run->dusk_zenith.r, 0.0F) << "dusk irradiance is not finite";
    EXPECT_GE(run->dusk_zenith.b, 0.0F) << "dusk irradiance is not finite";
    EXPECT_LE(run->dusk_zenith.b, run->noon_zenith.b * 0.8F);
}

namespace {

// R13.4: the probe bakes the v2 clouds of the layer it is given — an
// overcast deck overhead turns the zenith irradiance from sky blue toward
// cloud white (blue / red closer to 1), and the clear layer leaves it blue.
std::expected<std::array<glm::vec3, 2>, Error> clear_then_overcast(quat::test::GpuContext& g) {
    auto atmos = r::AtmospherePass::create(g.device, g.allocator, g.cache, g.shader_dir);
    if (!atmos) return std::unexpected(atmos.error());
    auto noise = r::CloudNoisePass::create(g.device, g.allocator, g.cache, g.shader_dir);
    if (!noise) return std::unexpected(noise.error());
    std::array<glm::vec3, 2> out{};
    for (int i = 0; i < 2; ++i) {
        auto p = lut_sky(*atmos, glm::vec3(0.3F, 1.0F, 0.3F));
        if (!p) return std::unexpected(p.error());
        p->clouds.enabled = true;
        r::CloudSettings c;
        c.coverage = i == 0 ? 0.0F : 1.0F;
        c.cirrus_coverage = 0.0F;
        p->layer = r::make_cloud_layer(c, false, glm::vec3(0.0F), r::CloudLights{.sun_to = p->sun_to});
        p->noise = &*noise;
        auto probe = r::DynamicIbl::create(g.device, g.allocator, g.cache, g.shader_dir, *p);
        if (!probe) return std::unexpected(probe.error());
        auto zenith = front_zenith(g, *probe);
        if (!zenith) return std::unexpected(zenith.error());
        out[static_cast<std::size_t>(i)] = *zenith;
    }
    return out;
}

} // namespace

TEST(GpuDynamicIbl, OvercastCloudsWhitenTheProbe) {
    QUAT_REQUIRE_GPU(g);
    auto z = clear_then_overcast(*g);
    ASSERT_OK(z);
    const glm::vec3 clear = (*z)[0];
    const glm::vec3 overcast = (*z)[1];
    ASSERT_GT(clear.r, 0.0F);
    ASSERT_GT(overcast.r, 0.0F);
    EXPECT_GT(clear.b / clear.r, 1.2F) << "clear: sky blue";
    EXPECT_LT(overcast.b / overcast.r, 0.85F * (clear.b / clear.r)) << "overcast: whiter";
}
