// Global wind field on the GPU (R13.1): WindFieldPass's texture matches the
// CPU reference (engine::weather::sample_wind) texel for texel.

#include <algorithm>
#include <cmath>

#include <glm/glm.hpp>
#include <gtest/gtest.h>

#include "engine/renderer/wind_field_pass.hpp"
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

TEST(WindFieldLayout, CornerIsCentredAndTexelSnapped) {
    const glm::vec2 a = r::wind_field_corner({10.3F, -7.9F});
    const glm::vec2 b = r::wind_field_corner({11.9F, -6.1F}); // same texel
    EXPECT_EQ(a, b);
    EXPECT_FLOAT_EQ(std::fmod(std::abs(a.x), r::wind_field_texel_m), 0.0F);
    const float half = 0.5F * r::wind_field_texel_m * r::wind_field_resolution;
    EXPECT_NEAR(a.x + half, 10.0F, 1e-4F);
    EXPECT_NEAR(a.y + half, -8.0F, 1e-4F);
}

TEST(GpuWindField, TextureMatchesTheCpuReference) {
    QUAT_REQUIRE_GPU(g);
    auto pass = r::WindFieldPass::create(g->device, g->allocator, g->cache, g->shader_dir);
    ASSERT_OK(pass);

    w::WeatherState s;
    s.wind_dir_deg = -62.0F;
    s.wind_speed = 17.0F;
    s.gustiness = 0.8F;
    s.turbulence = 0.7F;
    w::WindAdvection clock;
    clock.advance(17.0F, 3600.0F); // an hour of wind: offsets wrapped, still exact
    const w::WindFieldParams params = w::wind_field_params(s, clock);
    const glm::vec2 camera(-1234.5F, 987.25F);

    rhi::TransientImagePool pool(g->device, g->allocator);
    rhi::RenderGraph graph(pool);
    auto out = pass->add_to_graph(graph, params, camera);
    ASSERT_OK(out);
    constexpr VkExtent2D extent{r::wind_field_resolution, r::wind_field_resolution};
    auto px = quat::test::render_to_host(*g, graph, out->texture, extent, 8);
    ASSERT_OK(px);

    const glm::vec2 corner = r::wind_field_corner(camera);
    EXPECT_FLOAT_EQ(out->rect.x, corner.x);
    EXPECT_FLOAT_EQ(out->rect.z, 1.0F / (r::wind_field_texel_m * r::wind_field_resolution));
    float worst = 0.0F;
    float max_speed = 0.0F;
    for (std::uint32_t y = 0; y < extent.height; ++y) {
        for (std::uint32_t x = 0; x < extent.width; ++x) {
            const glm::vec2 xz = corner + (glm::vec2(static_cast<float>(x), static_cast<float>(y)) +
                                           0.5F) * r::wind_field_texel_m;
            const w::WindSample ref = w::sample_wind(params, xz);
            const glm::vec4 gpu = px->rgba16f(x, y);
            const glm::vec4 want(ref.velocity.x, ref.velocity.z, ref.velocity.y, ref.gust);
            // Half floats: ~1e-3 relative.
            const glm::vec4 err = glm::abs(gpu - want);
            const float tol = 2e-3F * glm::length(ref.velocity) + 2e-3F;
            worst = std::max(worst, std::max(std::max(err.x, err.y), std::max(err.z, err.w)) / tol);
            max_speed = std::max(max_speed, glm::length(ref.velocity));
        }
    }
    EXPECT_LE(worst, 1.0F) << "largest error / tolerance";
    EXPECT_GT(max_speed, 17.0F); // gusts present
}

} // namespace
