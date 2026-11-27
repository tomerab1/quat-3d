// Grass in the global wind field (R13.1): the blades read WindFieldPass's
// texture (bend grows with the wind and points downwind), a calm field and a
// zero-strength R9.3 sway both leave only the static lean, and the flutter
// phase stays put far from the origin when the turbulence turns the local
// wind. Seen from straight above (orthographic), where a blade's coverage is
// its horizontal extent.

#include <cmath>
#include <cstdint>
#include <expected>
#include <optional>
#include <vector>

#include <glm/glm.hpp>
#include <glm/gtc/matrix_transform.hpp>
#include <gtest/gtest.h>
#include <vulkan/vulkan.h>

#include "engine/renderer/grass.hpp"
#include "engine/renderer/grass_pass.hpp"
#include "engine/renderer/mesh_pass.hpp"
#include "engine/renderer/wind_field_pass.hpp"
#include "engine/rhi/gpu_allocator.hpp"
#include "engine/rhi/render_graph.hpp"
#include "engine/weather/weather_state.hpp"
#include "engine/weather/wind_field.hpp"
#include "support/expect_ok.hpp"
#include "support/gpu_context.hpp"
#include "support/render_readback.hpp"

namespace {

namespace r = engine::renderer;
namespace rhi = engine::rhi;
namespace w = engine::weather;
using engine::core::Error;

constexpr VkExtent2D k_extent{192, 192};
constexpr float      k_half_m = 3.0F; // the view spans 6 m

// Covered pixels (GBuffer albedo alpha) of the grass around `centre`.
struct Coverage {
    std::vector<std::uint8_t> mask;
    std::size_t               count = 0;
    glm::vec2                 centroid{0.0F}; // pixels (x right, y down the rows)
};

// A grass disc of `radius` around `centre` (inside the view when small, so
// its coverage moves with the blades), at flutter clock `time_s`.
[[nodiscard]] std::expected<Coverage, Error>
render_grass(quat::test::GpuContext& g, const glm::vec3& centre, float strength,
             const std::optional<w::WindFieldParams>& wind, float radius = 6.0F,
             float time_s = 3.0F) {
    auto mesh = r::MeshPass::create(g.device, g.allocator, g.cache, g.transfer, g.shader_dir);
    if (!mesh) return std::unexpected(mesh.error());
    auto grass = r::GrassPass::create(g.device, g.allocator, g.cache, g.transfer, g.shader_dir, 1);
    if (!grass) return std::unexpected(grass.error());
    auto wind_pass = r::WindFieldPass::create(g.device, g.allocator, g.cache, g.shader_dir);
    if (!wind_pass) return std::unexpected(wind_pass.error());

    r::GrassFrame frame;
    frame.settings.seed = 9;
    frame.settings.density = 60.0F;
    frame.settings.radius = radius;
    frame.settings.terrain_only = false;
    frame.settings.ground_y = centre.y;
    frame.settings.lod_start = 1.0F;
    frame.settings.far_density = 1.0F;
    frame.settings.wind_strength = strength;
    frame.settings.wind_direction = {0.0F, 1.0F};
    // Grass distance LOD measures from the camera: keep it at the ground.
    frame.camera = centre + glm::vec3(0.0F, 0.1F, 0.0F);
    const glm::mat4 view = glm::lookAt(centre + glm::vec3(0.0F, 50.0F, 0.0F), centre,
                                       glm::vec3(0.0F, 0.0F, 1.0F));
    frame.draw_view_proj =
        glm::ortho(-k_half_m, k_half_m, -k_half_m, k_half_m, 1.0F, 100.0F) * view;
    frame.cull_view_proj = frame.draw_view_proj;
    frame.time_s = time_s;

    rhi::TransientImagePool pool(g.device, g.allocator);
    rhi::RenderGraph graph(pool);
    if (wind) {
        auto field = wind_pass->add_to_graph(graph, *wind, glm::vec2(centre.x, centre.z));
        if (!field) return std::unexpected(field.error());
        frame.wind = *field;
    }
    auto gbuffer = mesh->add_to_graph(graph, k_extent, frame.draw_view_proj, r::InstancedDrawList{});
    if (!gbuffer) return std::unexpected(gbuffer.error());
    auto out = grass->add_to_graph(graph, 0, frame, &*gbuffer, k_extent);
    if (!out) return std::unexpected(out.error());
    auto px = quat::test::render_to_host(g, graph, gbuffer->albedo, k_extent, 4);
    if (!px) return std::unexpected(px.error());

    Coverage c;
    c.mask.resize(static_cast<std::size_t>(k_extent.width) * k_extent.height);
    glm::dvec2 sum(0.0);
    for (std::uint32_t y = 0; y < k_extent.height; ++y) {
        for (std::uint32_t x = 0; x < k_extent.width; ++x) {
            const bool covered = px->rgba8(x, y).a > 200;
            c.mask[static_cast<std::size_t>(y) * k_extent.width + x] = covered ? 1 : 0;
            if (covered) {
                ++c.count;
                sum += glm::dvec2(x, y);
            }
        }
    }
    if (c.count > 0) c.centroid = glm::vec2(sum / static_cast<double>(c.count));
    return c;
}

// Uniform wind toward `heading_deg` (no gusts, no swirl).
[[nodiscard]] w::WindFieldParams steady(float speed, float heading_deg) {
    w::WeatherState s;
    s.wind_speed = speed;
    s.wind_dir_deg = heading_deg;
    s.gustiness = 0.0F;
    s.turbulence = 0.0F;
    return w::wind_field_params(s, w::WindAdvection{});
}

[[nodiscard]] std::size_t differing(const Coverage& a, const Coverage& b) {
    std::size_t n = 0;
    for (std::size_t i = 0; i < a.mask.size(); ++i) n += a.mask[i] != b.mask[i] ? 1U : 0U;
    return n;
}

TEST(GpuGrassWind, BladesBendDownwindHarderInStrongerWind) {
    QUAT_REQUIRE_GPU(g);
    // A 1.5 m patch inside the 6 m view: its coverage moves with the tips.
    const glm::vec3 centre(0.0F);
    auto calm = render_grass(*g, centre, 1.0F, steady(0.0F, 90.0F), 1.5F);
    ASSERT_OK(calm);
    auto breeze = render_grass(*g, centre, 1.0F, steady(4.0F, 90.0F), 1.5F);
    ASSERT_OK(breeze);
    auto gale = render_grass(*g, centre, 1.0F, steady(15.0F, 90.0F), 1.5F);
    ASSERT_OK(gale);
    ASSERT_GT(calm->count, 200U);

    // The view's rows run along +Z (the wind's heading at 90 degrees) and its
    // columns along -X: blades bent toward +Z move the coverage down the rows.
    const float shift_breeze = breeze->centroid.y - calm->centroid.y;
    const float shift_gale = gale->centroid.y - calm->centroid.y;
    EXPECT_GT(shift_breeze, 1.0F) << "pixels";
    EXPECT_GT(shift_gale, shift_breeze + 1.0F) << "stronger wind bends further";
    EXPECT_LT(std::abs(gale->centroid.x - calm->centroid.x), 0.25F * shift_gale)
        << "no sideways drift in a wind along +Z";
    EXPECT_GT(gale->count, calm->count) << "bent blades cover more ground seen from above";
}

TEST(GpuGrassWind, CalmFieldLeavesOnlyTheStaticLean) {
    QUAT_REQUIRE_GPU(g);
    const glm::vec3 centre(0.0F);
    // A calm field, and the R9.3 sway (no field bound) with zero strength:
    // both bend nothing beyond each blade's static lean.
    auto calm_field = render_grass(*g, centre, 1.0F, steady(0.0F, 90.0F));
    ASSERT_OK(calm_field);
    auto still_legacy = render_grass(*g, centre, 0.0F, std::nullopt);
    ASSERT_OK(still_legacy);
    EXPECT_EQ(differing(*calm_field, *still_legacy), 0U);
    // The R9.3 sway is still what draws without a field.
    auto swaying_legacy = render_grass(*g, centre, 1.0F, std::nullopt);
    ASSERT_OK(swaying_legacy);
    EXPECT_GT(differing(*swaying_legacy, *still_legacy), 50U);
}

TEST(GpuGrassWind, FlutterPhaseIsStableFarFromTheOrigin) {
    QUAT_REQUIRE_GPU(g);
    // 2.5 km from the origin, in a turbulent wind: one frame later the swirl
    // has turned the local wind a little. The flutter's travelling wave
    // follows the mean heading, so the blades move only by that small turn.
    const glm::vec3 centre(2000.0F, 0.0F, 1500.0F);
    w::WeatherState s;
    s.wind_speed = 10.0F;
    s.wind_dir_deg = 20.0F;
    s.gustiness = 0.0F;
    s.turbulence = 1.0F;
    w::WindAdvection clock;
    clock.time_s = 40.0;
    const w::WindFieldParams before = w::wind_field_params(s, clock);
    w::WindFieldParams after = before;
    after.swirl_phase += 0.003F; // ~1/60 s of swirl evolution

    auto a = render_grass(*g, centre, 1.0F, before);
    ASSERT_OK(a);
    auto b = render_grass(*g, centre, 1.0F, after);
    ASSERT_OK(b);
    ASSERT_GT(a->count, 500U);
    // Blades are about one pixel wide, so even a sub-pixel move flips their
    // edge pixels: a stable phase changes a few percent of the coverage.
    const std::size_t turned = differing(*a, *b);
    EXPECT_LT(turned, a->count / 8) << "coverage pixels that flickered";
    // What a phase jump looks like (the flutter half a second later, a few
    // radians, as a phase taken from the turning local wind would jump): it
    // changes several times as much.
    auto jumped = render_grass(*g, centre, 1.0F, before, 6.0F, 3.5F);
    ASSERT_OK(jumped);
    EXPECT_GT(differing(*a, *jumped), 2 * turned);
}

} // namespace
