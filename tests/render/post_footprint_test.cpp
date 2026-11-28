// R20 depth of field and motion blur after TAAU (issue #115): the post passes
// run at the display extent but read the render-extent depth and velocity,
// which are jittered every frame. A static scene must not shimmer at depth
// edges as the jitter cycles.
//
// The scene is an in-focus disc in front of a defocused, checkered
// background (moving over it in the motion blur case), rendered
// at half the display resolution: each render texel samples the scene at its
// jittered position (jitter.hpp: render pixel i sees unjittered UV
// (i + 0.5 - jitter) / render), as the GBuffer does. The colour is the
// converged, antialiased display-extent image TAAU resolves for a static
// scene, so only the depth (and velocity) inputs change with the jitter.
// Over one Halton cycle of jitter the real DofPass / MotionBlurPass outputs
// are read back and the frame-to-frame variance at the pixels around the
// disc's silhouette is bounded. Reading the nearest jittered texel, a
// silhouette pixel flips between the sharp disc and the blurred background
// from one frame to the next; with the closest-depth footprint
// (lib/depth_footprint.slang) the disc keeps its silhouette in every frame.
// What remains is the background just outside it (within the one-texel
// dilation), which still alternates between its sharp and blurred colour.
//
// Measured on lavapipe (variance of the red channel over 32 jitters, mean /
// max over the pixels within 3 display px of the silhouette):
//   depth of field   nearest texel 0.0066 / 0.081, closest depth 0.00060 / 0.0035
//   motion blur      nearest texel 0.0029 / 0.054, closest depth 0.00063 / 0.010
// The bounds sit between the two, about 2-3x from each.

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <expected>
#include <utility>
#include <vector>

#include <glm/glm.hpp>
#include <glm/gtc/matrix_transform.hpp>
#include <gtest/gtest.h>
#include <vulkan/vulkan.h>

#include "engine/renderer/dof_pass.hpp"
#include "engine/renderer/jitter.hpp"
#include "engine/renderer/motion_blur_pass.hpp"
#include "engine/renderer/physical_camera.hpp"
#include "engine/rhi/render_graph.hpp"
#include "render/post_image.hpp"
#include "support/expect_ok.hpp"
#include "support/gpu_context.hpp"
#include "support/render_readback.hpp"

namespace {

namespace r = engine::renderer;
namespace rhi = engine::rhi;
using engine::core::Error;
using quat::test::HostImage;

constexpr VkExtent2D k_display{128, 64};
constexpr VkExtent2D k_render{64, 32}; // TAAU at 50 % render scale
constexpr float      k_fov_y = 0.6981317F; // 40 degrees
constexpr float      k_focus_m = 2.0F;
constexpr float      k_background_m = 50.0F;
// The disc, in display pixels (off the texel grid so every sub-pixel phase
// of its edge occurs somewhere along the silhouette).
constexpr glm::vec2 k_centre{64.3F, 32.4F};
constexpr float     k_radius = 19.7F;
constexpr float     k_disc = 1.0F;
// The background: a 4 px checker of two greys, so blurring it changes it.
constexpr float     k_background_dark = 0.2F;
constexpr float     k_background_light = 0.4F;

struct Proj {
    float p22 = 0.0F;
    float p32 = 0.0F;
    [[nodiscard]] float depth_of(float z) const { return -p22 + p32 / z; }
};
Proj make_proj() {
    const glm::mat4 p = glm::perspective(k_fov_y, 2.0F, 0.1F, 1000.0F);
    return {p[2][2], p[3][2]};
}

[[nodiscard]] bool in_disc(glm::vec2 display_px) {
    const glm::vec2 d = display_px - k_centre;
    return glm::dot(d, d) < k_radius * k_radius;
}

[[nodiscard]] float scene_color(glm::vec2 display_px) {
    if (in_disc(display_px)) return k_disc;
    const auto cell = glm::ivec2(glm::floor(display_px / 4.0F));
    return ((cell.x + cell.y) & 1) != 0 ? k_background_light : k_background_dark;
}

// The converged TAAU output of the static scene: 8 x 8 coverage samples
// per display pixel.
std::vector<glm::vec4> resolved_color() {
    std::vector<glm::vec4> out(static_cast<std::size_t>(k_display.width) * k_display.height);
    for (std::uint32_t y = 0; y < k_display.height; ++y) {
        for (std::uint32_t x = 0; x < k_display.width; ++x) {
            float sum = 0.0F;
            for (int sy = 0; sy < 8; ++sy) {
                for (int sx = 0; sx < 8; ++sx) {
                    const glm::vec2 p(static_cast<float>(x) + (static_cast<float>(sx) + 0.5F) / 8.0F,
                                      static_cast<float>(y) + (static_cast<float>(sy) + 0.5F) / 8.0F);
                    sum += scene_color(p);
                }
            }
            const float c = sum / 64.0F;
            out[y * k_display.width + x] = glm::vec4(c, c, c, 1.0F);
        }
    }
    return out;
}

// The render-extent texels under `jitter`: each samples the scene point at
// its jittered position. Returns whether each texel landed on the disc.
std::vector<bool> jittered_disc(glm::vec2 jitter) {
    std::vector<bool> out(static_cast<std::size_t>(k_render.width) * k_render.height);
    const glm::vec2 scale(static_cast<float>(k_display.width) / static_cast<float>(k_render.width),
                          static_cast<float>(k_display.height) / static_cast<float>(k_render.height));
    for (std::uint32_t y = 0; y < k_render.height; ++y) {
        for (std::uint32_t x = 0; x < k_render.width; ++x) {
            const glm::vec2 sample = (glm::vec2(x, y) + 0.5F - jitter) * scale;
            out[y * k_render.width + x] = in_disc(sample);
        }
    }
    return out;
}

// Frame-to-frame variance of the red channel per display pixel, over frames.
struct EdgeVariance {
    double mean = 0.0; // over the silhouette pixels
    double max = 0.0;
};
EdgeVariance silhouette_variance(const std::vector<HostImage>& frames) {
    EdgeVariance out;
    int count = 0;
    for (std::uint32_t y = 0; y < k_display.height; ++y) {
        for (std::uint32_t x = 0; x < k_display.width; ++x) {
            const glm::vec2 c(static_cast<float>(x) + 0.5F, static_cast<float>(y) + 0.5F);
            // Pixels within 3 display px (1.5 render texels) of the silhouette.
            if (std::abs(glm::length(c - k_centre) - k_radius) > 3.0F) continue;
            double sum = 0.0;
            double sum2 = 0.0;
            for (const HostImage& f : frames) {
                const double v = f.rgba16f(x, y).r;
                sum += v;
                sum2 += v * v;
            }
            const double n = static_cast<double>(frames.size());
            const double var = std::max(sum2 / n - (sum / n) * (sum / n), 0.0);
            out.mean += var;
            out.max = std::max(out.max, var);
            ++count;
        }
    }
    out.mean /= std::max(count, 1);
    return out;
}

std::vector<r::ViewJitter> jitter_cycle() {
    const std::uint32_t phases = r::jitter_phase_count(k_render, k_display);
    std::vector<r::ViewJitter> out;
    for (std::uint32_t i = 0; i < phases; ++i) out.push_back(r::halton_jitter(i, k_render, phases));
    return out;
}

// The test's premise: over the cycle, the jitter moves render texels on and
// off the disc (the inputs the passes see really change).
TEST(PostFootprint, JitterMovesTheSilhouetteTexels) {
    const std::vector<r::ViewJitter> cycle = jitter_cycle();
    ASSERT_EQ(cycle.size(), 32U); // 8 phases x the 4x pixel-area ratio
    const std::vector<bool> first = jittered_disc(cycle.front().pixels);
    std::size_t changed = 0;
    for (const r::ViewJitter& j : cycle) {
        const std::vector<bool> texels = jittered_disc(j.pixels);
        for (std::size_t i = 0; i < texels.size(); ++i) changed += texels[i] != first[i] ? 1 : 0;
    }
    EXPECT_GT(changed, 100U);
}

// 135 mm f/1.4 focused at 2 m: the background's CoC radius is ~9 display px.
r::PhysicalCamera lens() {
    r::PhysicalCamera c;
    c.focal_length_mm = 135.0F;
    c.aperture_f = 1.4F;
    c.focus_distance_m = k_focus_m;
    return c;
}

std::expected<std::vector<HostImage>, Error> render_dof_cycle(quat::test::GpuContext& g) {
    const Proj proj = make_proj();
    auto color = quat::test::upload_rgba16f(g, k_display, resolved_color());
    if (!color) return std::unexpected(color.error());
    auto pass = r::DofPass::create(g.device, g.allocator, g.cache, g.shader_dir);
    if (!pass) return std::unexpected(pass.error());
    std::vector<HostImage> frames;
    for (const r::ViewJitter& j : jitter_cycle()) {
        const std::vector<bool> disc = jittered_disc(j.pixels);
        std::vector<float> depth(disc.size());
        for (std::size_t i = 0; i < disc.size(); ++i) {
            depth[i] = proj.depth_of(disc[i] ? k_focus_m : k_background_m);
        }
        auto depth_img = quat::test::upload_r32f(g, k_render, depth);
        if (!depth_img) return std::unexpected(depth_img.error());
        rhi::TransientImagePool pool(g.device, g.allocator);
        rhi::RenderGraph graph(pool);
        r::DofInputs in;
        in.color = color->import(graph, "test_color");
        in.depth = depth_img->import(graph, "test_depth");
        in.display_extent = k_display;
        in.render_extent = k_render;
        in.jitter = j.pixels;
        in.coc = r::coc_params(lens(), k_fov_y, static_cast<float>(k_display.height));
        in.max_radius_px = 24.0F;
        in.rings = 4;
        in.proj22 = proj.p22;
        in.proj32 = proj.p32;
        auto out = pass->add_to_graph(graph, in);
        if (!out) return std::unexpected(out.error());
        auto img = quat::test::render_to_host(g, graph, *out, k_display, 8);
        if (!img) return std::unexpected(img.error());
        frames.push_back(std::move(*img));
    }
    return frames;
}

// The disc moves right by 12 display px per frame over a still background.
constexpr float k_motion_px = 12.0F;

std::expected<std::vector<HostImage>, Error> render_motion_blur_cycle(quat::test::GpuContext& g) {
    const Proj proj = make_proj();
    auto color = quat::test::upload_rgba16f(g, k_display, resolved_color());
    if (!color) return std::unexpected(color.error());
    auto pass = r::MotionBlurPass::create(g.device, g.allocator, g.cache, g.shader_dir);
    if (!pass) return std::unexpected(pass.error());
    const glm::vec2 moving(k_motion_px / static_cast<float>(k_display.width), 0.0F);
    std::vector<HostImage> frames;
    for (const r::ViewJitter& j : jitter_cycle()) {
        const std::vector<bool> disc = jittered_disc(j.pixels);
        std::vector<float>     depth(disc.size());
        std::vector<glm::vec2> velocity(disc.size());
        for (std::size_t i = 0; i < disc.size(); ++i) {
            depth[i] = proj.depth_of(disc[i] ? k_focus_m : k_background_m);
            velocity[i] = disc[i] ? moving : glm::vec2(0.0F);
        }
        auto depth_img = quat::test::upload_r32f(g, k_render, depth);
        if (!depth_img) return std::unexpected(depth_img.error());
        auto velocity_img = quat::test::upload_rg16f(g, k_render, velocity);
        if (!velocity_img) return std::unexpected(velocity_img.error());
        rhi::TransientImagePool pool(g.device, g.allocator);
        rhi::RenderGraph graph(pool);
        r::MotionBlurInputs in;
        in.color = color->import(graph, "test_color");
        in.velocity = velocity_img->import(graph, "test_velocity");
        in.depth = depth_img->import(graph, "test_depth");
        in.display_extent = k_display;
        in.render_extent = k_render;
        in.jitter = j.pixels;
        in.shutter_fraction = 0.5F; // 180 degrees: +-3 px around each pixel
        in.max_radius_px = 16.0F;
        in.samples = 16;
        in.proj22 = proj.p22;
        in.proj32 = proj.p32;
        // The sampling noise is held still, so only the jittered inputs vary.
        in.frame_index = 0;
        auto out = pass->add_to_graph(graph, in);
        if (!out) return std::unexpected(out.error());
        auto img = quat::test::render_to_host(g, graph, *out, k_display, 8);
        if (!img) return std::unexpected(img.error());
        frames.push_back(std::move(*img));
    }
    return frames;
}

TEST(GpuPostFootprint, DofSilhouetteIsStableUnderJitter) {
    QUAT_REQUIRE_GPU(g);
    auto frames = render_dof_cycle(*g);
    ASSERT_OK(frames);
    ASSERT_GE(frames->size(), 16U);
    const EdgeVariance v = silhouette_variance(*frames);
    std::printf("[dof] silhouette variance over %zu jitters: mean %.6f, max %.6f\n",
                frames->size(), v.mean, v.max);
    EXPECT_LT(v.mean, 2e-3);
    EXPECT_LT(v.max, 1e-2);
}

TEST(GpuPostFootprint, MotionBlurSilhouetteIsStableUnderJitter) {
    QUAT_REQUIRE_GPU(g);
    auto frames = render_motion_blur_cycle(*g);
    ASSERT_OK(frames);
    ASSERT_GE(frames->size(), 16U);
    const EdgeVariance v = silhouette_variance(*frames);
    std::printf("[motion blur] silhouette variance over %zu jitters: mean %.6f, max %.6f\n",
                frames->size(), v.mean, v.max);
    EXPECT_LT(v.mean, 1.5e-3);
    EXPECT_LT(v.max, 2.5e-2);
}

} // namespace
