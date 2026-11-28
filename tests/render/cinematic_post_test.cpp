// R20 depth of field and motion blur on the GPU, on synthetic inputs: the
// colour, depth and velocity targets are uploaded images, so each test knows
// exactly what the passes see.
//
//   * DoF: a point at the focus distance stays sharp, one far behind it
//     spreads over its circle of confusion (energy kept), one in front of it
//     spreads too (near field);
//   * motion blur: a line moving sideways blurs along the motion only, over
//     the half vector, and a still image is untouched.

#include <array>
#include <cmath>
#include <cstdint>
#include <expected>
#include <vector>

#include <glm/glm.hpp>
#include <glm/gtc/matrix_transform.hpp>
#include <gtest/gtest.h>
#include <vulkan/vulkan.h>

#include "engine/renderer/dof_pass.hpp"
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

// Projection terms of a perspective camera (depth [0, 1], 1 = far).
struct Proj {
    float p22 = 0.0F;
    float p32 = 0.0F;
    [[nodiscard]] float depth_of(float z) const { return -p22 + p32 / z; }
};
Proj make_proj() {
    const glm::mat4 p = glm::perspective(glm::radians(40.0F), 2.0F, 0.1F, 1000.0F);
    return {p[2][2], p[3][2]};
}

float red(const HostImage& img, std::uint32_t x, std::uint32_t y) { return img.rgba16f(x, y).r; }

// ---- Depth of field ---------------------------------------------------------

constexpr VkExtent2D k_dof_extent{128, 64};
constexpr float      k_focus_m = 2.0F;
constexpr float      k_far_m = 50.0F;
constexpr float      k_near_m = 0.8F;
constexpr float      k_dot = 50.0F;
constexpr glm::uvec2 k_sharp_dot{16, 32};
constexpr glm::uvec2 k_far_dot{52, 32};
constexpr glm::uvec2 k_near_dot{100, 32};

// 135 mm f/1.4 focused at 2 m on a 24 mm sensor, 64 px tall: the far CoC
// radius is ~9 px, the near one (0.8 m) ~14 px.
r::PhysicalCamera dof_lens() {
    r::PhysicalCamera c;
    c.focal_length_mm = 135.0F;
    c.aperture_f = 1.4F;
    c.focus_distance_m = k_focus_m;
    return c;
}

std::expected<HostImage, Error> render_dof(quat::test::GpuContext& g) {
    const Proj proj = make_proj();
    const std::uint32_t w = k_dof_extent.width;
    const std::uint32_t h = k_dof_extent.height;
    std::vector<glm::vec4> color(static_cast<std::size_t>(w) * h, glm::vec4(0.0F, 0.0F, 0.0F, 1.0F));
    std::vector<float>     depth(color.size());
    for (std::uint32_t y = 0; y < h; ++y) {
        for (std::uint32_t x = 0; x < w; ++x) {
            const float z = x < 36 ? k_focus_m : x < 76 ? k_far_m : k_near_m;
            depth[y * w + x] = proj.depth_of(z);
        }
    }
    for (const glm::uvec2 p : {k_sharp_dot, k_far_dot, k_near_dot}) {
        color[p.y * w + p.x] = glm::vec4(k_dot, k_dot, k_dot, 1.0F);
    }
    auto color_img = quat::test::upload_rgba16f(g, k_dof_extent, color);
    if (!color_img) return std::unexpected(color_img.error());
    auto depth_img = quat::test::upload_r32f(g, k_dof_extent, depth);
    if (!depth_img) return std::unexpected(depth_img.error());
    auto pass = r::DofPass::create(g.device, g.allocator, g.cache, g.shader_dir);
    if (!pass) return std::unexpected(pass.error());

    rhi::TransientImagePool pool(g.device, g.allocator);
    rhi::RenderGraph graph(pool);
    r::DofInputs in;
    in.color = color_img->import(graph, "test_color");
    in.depth = depth_img->import(graph, "test_depth");
    in.display_extent = k_dof_extent;
    in.render_extent = k_dof_extent;
    in.coc = r::coc_params(dof_lens(), glm::radians(40.0F), static_cast<float>(h));
    in.max_radius_px = 24.0F;
    in.rings = 4;
    in.proj22 = proj.p22;
    in.proj32 = proj.p32;
    auto out = pass->add_to_graph(graph, in);
    if (!out) return std::unexpected(out.error());
    return quat::test::render_to_host(g, graph, *out, k_dof_extent, 8);
}

const std::expected<HostImage, Error>& dof_frame(quat::test::GpuContext& g) {
    static const std::expected<HostImage, Error> image = render_dof(g);
    return image;
}

float sum_around(const HostImage& img, glm::uvec2 c, int radius) {
    float s = 0.0F;
    for (int y = -radius; y <= radius; ++y) {
        for (int x = -radius; x <= radius; ++x) {
            s += red(img, static_cast<std::uint32_t>(static_cast<int>(c.x) + x),
                     static_cast<std::uint32_t>(static_cast<int>(c.y) + y));
        }
    }
    return s;
}

TEST(GpuDof, PointAtTheFocusDistanceStaysSharp) {
    QUAT_REQUIRE_GPU(g);
    const auto& img = dof_frame(*g);
    ASSERT_OK(img);
    EXPECT_NEAR(red(*img, k_sharp_dot.x, k_sharp_dot.y), k_dot, k_dot * 0.01F);
    for (const glm::ivec2 d : {glm::ivec2(2, 0), glm::ivec2(0, 2), glm::ivec2(-3, 1)}) {
        EXPECT_LT(red(*img, k_sharp_dot.x + d.x, k_sharp_dot.y + d.y), 0.01F) << d.x << "," << d.y;
    }
}

TEST(GpuDof, FarPointSpreadsOverItsCircleOfConfusion) {
    QUAT_REQUIRE_GPU(g);
    const auto& img = dof_frame(*g);
    ASSERT_OK(img);
    const float radius = r::coc_params(dof_lens(), glm::radians(40.0F), 64.0F).radius_px(k_far_m);
    ASSERT_GT(radius, 7.0F);
    const glm::uvec2 c = k_far_dot;
    EXPECT_LT(red(*img, c.x, c.y), k_dot * 0.1F); // the peak is gone
    // Spread in every direction, about as far as the CoC.
    for (const glm::ivec2 d : {glm::ivec2(5, 0), glm::ivec2(-5, 0), glm::ivec2(0, 5), glm::ivec2(0, -5)}) {
        EXPECT_GT(red(*img, c.x + d.x, c.y + d.y), 0.01F) << d.x << "," << d.y;
    }
    EXPECT_LT(red(*img, c.x + 15, c.y), 0.01F);
    EXPECT_LT(red(*img, c.x, c.y + 15), 0.01F);
    // A gather keeps the point's energy (within the sampling error).
    const float energy = sum_around(*img, c, 14);
    EXPECT_GT(energy, k_dot * 0.5F);
    EXPECT_LT(energy, k_dot * 1.6F);
}

TEST(GpuDof, NearPointBlursInTheNearField) {
    QUAT_REQUIRE_GPU(g);
    const auto& img = dof_frame(*g);
    ASSERT_OK(img);
    const glm::uvec2 c = k_near_dot;
    EXPECT_LT(red(*img, c.x, c.y), k_dot * 0.1F);
    EXPECT_GT(red(*img, c.x + 6, c.y), 0.005F);
    EXPECT_GT(red(*img, c.x, c.y - 6), 0.005F);
}

// ---- Motion blur ------------------------------------------------------------

constexpr VkExtent2D k_mb_extent{64, 64};
constexpr float      k_line = 10.0F;
constexpr std::uint32_t k_line_x = 32;
constexpr std::uint32_t k_row_y = 50;

// A vertical bright line at x = 32 and a horizontal one at y = 50, moving
// right by `motion_px` per frame.
std::expected<HostImage, Error> render_motion_blur(quat::test::GpuContext& g, float motion_px) {
    const std::uint32_t w = k_mb_extent.width;
    const std::uint32_t h = k_mb_extent.height;
    std::vector<glm::vec4> color(static_cast<std::size_t>(w) * h, glm::vec4(0.0F, 0.0F, 0.0F, 1.0F));
    for (std::uint32_t y = 0; y < h; ++y) color[y * w + k_line_x] = glm::vec4(k_line, k_line, k_line, 1.0F);
    for (std::uint32_t x = 0; x < w; ++x) color[k_row_y * w + x] = glm::vec4(k_line, k_line, k_line, 1.0F);
    const std::vector<glm::vec2> velocity(color.size(), glm::vec2(motion_px / static_cast<float>(w), 0.0F));
    const Proj proj = make_proj();
    const std::vector<float> depth(color.size(), proj.depth_of(5.0F));
    auto color_img = quat::test::upload_rgba16f(g, k_mb_extent, color);
    if (!color_img) return std::unexpected(color_img.error());
    auto velocity_img = quat::test::upload_rg16f(g, k_mb_extent, velocity);
    if (!velocity_img) return std::unexpected(velocity_img.error());
    auto depth_img = quat::test::upload_r32f(g, k_mb_extent, depth);
    if (!depth_img) return std::unexpected(depth_img.error());
    auto pass = r::MotionBlurPass::create(g.device, g.allocator, g.cache, g.shader_dir);
    if (!pass) return std::unexpected(pass.error());

    rhi::TransientImagePool pool(g.device, g.allocator);
    rhi::RenderGraph graph(pool);
    r::MotionBlurInputs in;
    in.color = color_img->import(graph, "test_color");
    in.velocity = velocity_img->import(graph, "test_velocity");
    in.depth = depth_img->import(graph, "test_depth");
    in.display_extent = k_mb_extent;
    in.render_extent = k_mb_extent;
    in.shutter_fraction = 1.0F; // 360 degrees: blur = the whole frame's motion
    in.max_radius_px = 16.0F;
    in.samples = 16;
    in.proj22 = proj.p22;
    in.proj32 = proj.p32;
    auto out = pass->add_to_graph(graph, in);
    if (!out) return std::unexpected(out.error());
    return quat::test::render_to_host(g, graph, *out, k_mb_extent, 8);
}

TEST(GpuMotionBlur, BlursAlongTheVelocityOnly) {
    QUAT_REQUIRE_GPU(g);
    // 8 px of motion over the shutter: the blur spans +-4 px.
    auto img = render_motion_blur(*g, 8.0F);
    ASSERT_OK(img);
    const std::uint32_t y = 20;
    EXPECT_LT(red(*img, k_line_x, y), k_line * 0.6F);
    EXPECT_GT(red(*img, k_line_x + 2, y), k_line * 0.05F);
    EXPECT_GT(red(*img, k_line_x - 2, y), k_line * 0.05F);
    EXPECT_LT(red(*img, k_line_x + 7, y), 0.01F);
    EXPECT_LT(red(*img, k_line_x - 7, y), 0.01F);
    // Energy along the row is kept.
    float row = 0.0F;
    for (std::uint32_t x = k_line_x - 8; x <= k_line_x + 8; ++x) row += red(*img, x, y);
    EXPECT_NEAR(row, k_line, k_line * 0.3F);
    // Nothing moves vertically: the horizontal line keeps its rows.
    EXPECT_NEAR(red(*img, 10, k_row_y), k_line, k_line * 0.05F);
    EXPECT_LT(red(*img, 10, k_row_y + 2), 0.01F);
    EXPECT_LT(red(*img, 10, k_row_y - 2), 0.01F);
}

TEST(GpuMotionBlur, StillImageIsUntouched) {
    QUAT_REQUIRE_GPU(g);
    auto img = render_motion_blur(*g, 0.0F);
    ASSERT_OK(img);
    for (std::uint32_t y = 0; y < k_mb_extent.height; y += 7) {
        for (std::uint32_t x = 0; x < k_mb_extent.width; ++x) {
            const float expected = (x == k_line_x || y == k_row_y) ? k_line : 0.0F;
            ASSERT_EQ(red(*img, x, y), expected) << x << "," << y;
        }
    }
}

} // namespace
