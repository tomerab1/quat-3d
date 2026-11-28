// Camera cuts (R11 review): FrameRenderer::reset_temporal() resets its
// TemporalView, and the frame after it must neither reproject nor blend
// across the cut. Driven the way the frame loop drives it: TemporalView
// supplies the frame's ViewMotion, the objects' previous poses and the
// upscaler's reset flag.

#include <cstdint>
#include <memory>

#include <glm/glm.hpp>
#include <glm/gtc/matrix_transform.hpp>
#include <gtest/gtest.h>

#include "engine/renderer/draw_batching.hpp"
#include "engine/renderer/motion_history.hpp"
#include "engine/renderer/taa_pass.hpp"
#include "render/temporal_rig.hpp"
#include "render/velocity_rig.hpp"
#include "support/expect_ok.hpp"
#include "support/gpu_context.hpp"

namespace {

namespace r = engine::renderer;
namespace rhi = engine::rhi;
namespace vel = quat::test::velocity;

constexpr std::uint64_t k_quad = 1;

glm::mat4 quad_pose(int frame) {
    return glm::translate(glm::mat4(1.0F), glm::vec3(0.05F * static_cast<float>(frame), 0.0F, 0.0F));
}

glm::mat4 camera_at(int frame) {
    return vel::camera(glm::vec3(0.1F * static_cast<float>(frame), 0.0F, 3.0F));
}

} // namespace

// Before the cut a moving quad under a moving camera has motion; on the frame
// after reset() (the camera and the quad jumped) the whole target is
// camera-only with no previous view: zero everywhere, background included.
TEST(GpuTemporalReset, VelocityIsCameraOnlyOnTheResetFrame) {
    QUAT_REQUIRE_GPU(g);
    auto mesh = vel::quad(*g);
    ASSERT_OK(mesh);
    r::TemporalView view;
    const auto render = [&](int frame) {
        const glm::mat4 vp = camera_at(frame);
        const r::ViewMotion motion = view.begin_frame(vp, glm::vec2(0.0F));
        r::DrawItem item;
        item.mesh = &*mesh;
        item.model = quad_pose(frame);
        item.prev_model = view.motion().advance(k_quad, item.model);
        return vel::render_velocity(*g, r::batch_draws({&item, 1}), vp, vp, motion);
    };

    for (int frame = 0; frame < 3; ++frame) ASSERT_OK(render(frame));
    auto before = render(3);
    ASSERT_OK(before);
    const glm::uvec2 c = vel::pixel_of(glm::vec3(quad_pose(3)[3]), camera_at(3));
    EXPECT_GT(glm::length(vel::velocity_at(*before, c.x, c.y)), 1e-3F);
    EXPECT_GT(glm::length(vel::velocity_at(*before, 1, 1)), 1e-3F) << "background camera motion";
    EXPECT_FALSE(view.take_history_reset());

    view.reset();
    constexpr int k_cut = 20; // camera and quad jump
    auto after = render(k_cut);
    ASSERT_OK(after);
    EXPECT_TRUE(view.take_history_reset());
    const glm::uvec2 q = vel::pixel_of(glm::vec3(quad_pose(k_cut)[3]), camera_at(k_cut));
    for (const glm::uvec2 p : {q, glm::uvec2(1, 1), glm::uvec2(62, 40)}) {
        const glm::vec2 v = vel::velocity_at(*after, p.x, p.y);
        // Previous and current positions come through different matrix
        // products, so allow float rounding (one pixel is 1/64 here).
        EXPECT_LT(glm::length(v), 1e-5F) << "motion across the cut at " << p.x << ", " << p.y;
    }
}

// The upscale on the reset frame ignores the accumulated history: its output
// is exactly the current frame. Control: the same frames without the cut blend.
TEST(GpuTemporalReset, ResetFrameResolvesUnblended) {
    QUAT_REQUIRE_GPU(g);
    constexpr VkExtent2D extent{16, 16};
    auto rig = quat::test::TemporalRig::create(*g, extent, extent);
    ASSERT_OK(rig);

    // Six frames of flat 0.2, then a checker of 0.8 / 0.2: its neighbourhood
    // still admits the old history, so only the reset keeps the centre at 0.8.
    const auto centre_after = [&](bool cut) -> float {
        auto up = r::TaaUpscaler::create(g->device, g->allocator, g->cache, g->shader_dir, 2);
        if (!up) return -1.0F;
        r::TemporalView view;
        float centre = -1.0F;
        for (std::uint32_t i = 0; i < 7; ++i) {
            if (i == 6 && cut) view.reset();
            quat::test::TemporalFrame f;
            for (std::uint32_t y = 0; y < extent.height; ++y) {
                for (std::uint32_t x = 0; x < extent.width; ++x) {
                    const float v = (i == 6 && (x + y) % 2 == 0) ? 0.8F : 0.2F;
                    f.color.emplace_back(v, v, v, 1.0F);
                }
            }
            (void)view.begin_frame(glm::mat4(1.0F), glm::vec2(0.0F));
            f.reset = view.take_history_reset();
            auto out = rig->frame(f, [&up, i](rhi::RenderGraph& graph, const r::UpscalerInputs& in) {
                return (*up)->add_to_graph(graph, i % 2, in);
            });
            if (!out) return -1.0F;
            centre = quat::test::grey(*out, 8, 8);
        }
        return centre;
    };
    EXPECT_NEAR(centre_after(true), 0.8F, 3e-3F) << "history blended across the cut";
    const float blended = centre_after(false);
    EXPECT_GT(blended, 0.0F);
    EXPECT_LT(blended, 0.75F) << "control unexpectedly unblended";
}
