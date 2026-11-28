// Temporal anti-aliasing (TAAU at native resolution) across frames (audit S1
// A1/A2/A14, R11 velocity reprojection).
//
// Frames are driven the way the engine's frame loop drives them: two
// frame-in-flight slots of one TaaUpscaler sharing one history, alternating
// every frame. The current frame is a checkerboard whose phase flips every
// frame (what sub-pixel jitter does to detail finer than a pixel): a working
// history accumulates it, a broken one shows the raw alternation. Uniform
// frames check the neighbourhood clamp on flat regions; a moving square checks
// that reprojecting with the velocity target removes the ghosting that
// camera-only (depth) reprojection leaves behind moving objects.
//
// The resolve blends in exposed, Reinhard-compressed space (y = g / (1 + g)
// for a grey g at exposure 1), so expected values are computed through it.

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <limits>
#include <memory>
#include <string>
#include <vector>

#include <glm/glm.hpp>
#include <gtest/gtest.h>
#include <vulkan/vulkan.h>

#include "engine/renderer/non_finite_counters.hpp"
#include "engine/renderer/taa_pass.hpp"
#include "render/temporal_rig.hpp"
#include "support/expect_ok.hpp"
#include "support/gpu_context.hpp"

namespace {

namespace r = engine::renderer;
namespace rhi = engine::rhi;
using quat::test::HostImage;
using quat::test::TemporalFrame;
using quat::test::TemporalRig;

constexpr float k_hi = 0.8F;
constexpr float k_lo = 0.2F;
// The resolve keeps a running average over the samples that landed on the
// pixel, capped at 10: the n-th frame since a reset (from 0) blends the current
// frame in with weight 1 / min(n + 1, 10). Without jitter every sample lands
// on its pixel (confidence 1).
float current_weight(int n) { return 1.0F / static_cast<float>(std::min(n + 1, 10)); }
// RGBA16F round trip + resolve-space conversions.
constexpr float k_exact = 3e-3F;

float to_y(float g) { return g / (1.0F + g); }
float from_y(float y) { return y / (1.0F - y); }

// Checkerboard: k_hi where (x + y + phase) is even, k_lo elsewhere.
TemporalFrame checker(VkExtent2D e, std::uint32_t phase) {
    TemporalFrame f;
    f.color.resize(static_cast<std::size_t>(e.width) * e.height);
    for (std::uint32_t y = 0; y < e.height; ++y) {
        for (std::uint32_t x = 0; x < e.width; ++x) {
            const float v = ((x + y + phase) % 2 == 0) ? k_hi : k_lo;
            f.color[static_cast<std::size_t>(y) * e.width + x] = glm::vec4(v, v, v, 1.0F);
        }
    }
    return f;
}

TemporalFrame uniform(VkExtent2D e, float v) {
    TemporalFrame f;
    f.color.assign(static_cast<std::size_t>(e.width) * e.height, glm::vec4(v, v, v, 1.0F));
    return f;
}

// Largest per-channel (rgb) difference between an output and a frame's input.
float max_abs_diff(const HostImage& out, const TemporalFrame& f) {
    float worst = 0.0F;
    for (std::uint32_t y = 0; y < out.extent.height; ++y) {
        for (std::uint32_t x = 0; x < out.extent.width; ++x) {
            const glm::vec3 d = glm::abs(glm::vec3(out.rgba16f(x, y)) -
                                         glm::vec3(f.color[y * out.extent.width + x]));
            worst = std::max({worst, d.x, d.y, d.z});
        }
    }
    return worst;
}

std::unique_ptr<r::TaaUpscaler> make_upscaler(quat::test::GpuContext& g) {
    auto up = r::TaaUpscaler::create(g.device, g.allocator, g.cache, g.shader_dir, 2);
    return up ? std::move(*up) : nullptr;
}

// Resolves with slot `slot` of `up`.
auto with(r::Upscaler& up, std::uint32_t slot) {
    return [&up, slot](rhi::RenderGraph& graph, const r::UpscalerInputs& in) {
        return up.add_to_graph(graph, slot, in);
    };
}

// One upscaler resolving uniform grey frames at native 32x32 (zero velocity,
// no jitter); returns the centre value of each output.
std::vector<float> resolve_uniform_frames(quat::test::GpuContext& g,
                                          const std::vector<float>& greys) {
    constexpr VkExtent2D extent{32, 32};
    auto rig = TemporalRig::create(g, extent, extent);
    auto up = make_upscaler(g);
    std::vector<float> centres;
    if (!rig || !up) return centres;
    for (std::size_t i = 0; i < greys.size(); ++i) {
        auto out = rig->frame(uniform(extent, greys[i]), with(*up, static_cast<std::uint32_t>(i % 2)));
        if (!out) return centres;
        centres.push_back(quat::test::grey(*out, 16, 16));
    }
    return centres;
}

} // namespace

// A2: each frame must read the history the previous frame wrote, even though
// consecutive frames are recorded by different slots. With per-slot history
// every slot read a never-written image and the output was the raw input.
TEST(GpuTaa, HistoryAccumulatesAcrossFrameSlots) {
    QUAT_REQUIRE_GPU(g);
    constexpr VkExtent2D extent{16, 16};
    auto rig = TemporalRig::create(*g, extent, extent);
    ASSERT_OK(rig);
    auto up = make_upscaler(*g);
    ASSERT_NE(up, nullptr);

    constexpr int k_frames = 48;
    std::vector<float> centre;
    float expected = 0.0F; // CPU model of the centre pixel, in resolve space
    for (int i = 0; i < k_frames; ++i) {
        const TemporalFrame input = checker(extent, static_cast<std::uint32_t>(i));
        auto o = rig->frame(input, with(*up, static_cast<std::uint32_t>(i % 2)));
        ASSERT_OK(o) << "frame " << i;
        if (i == 0) EXPECT_LE(max_abs_diff(*o, input), k_exact) << "the first frame has no history";
        const float cur = input.color[8 * extent.width + 8].r;
        expected = glm::mix(expected, to_y(cur), current_weight(i));
        centre.push_back(quat::test::grey(*o, 8, 8));
    }
    EXPECT_EQ(up->history().frame_count(), static_cast<std::uint64_t>(k_frames));
    EXPECT_TRUE(up->history().valid());

    // Frame 1 (slot B) blends with frame 0's output (slot A); a broken history
    // shows the raw 0.2.
    EXPECT_NEAR(centre[1], from_y(glm::mix(to_y(k_hi), to_y(k_lo), current_weight(1))), 0.01F)
        << "frame 1 did not read frame 0's history";
    // Converged: the alternating input accumulates to its (resolve-space)
    // mean, oscillating only slightly.
    EXPECT_NEAR(centre[k_frames - 1], from_y(expected), 0.01F);
    EXPECT_LE(std::abs(centre[k_frames - 1] - centre[k_frames - 2]), 0.08F)
        << "history did not converge (input swings 0.6)";
}

// A14 / R11: with zero velocity a static image reprojects exactly onto itself
// (no resampling blur). The control run shows the test is sensitive: a
// velocity of half a pixel samples the history between texels and blurs it.
TEST(GpuTaa, StaticImageWithZeroVelocityStaysExact) {
    QUAT_REQUIRE_GPU(g);
    constexpr VkExtent2D extent{16, 16};
    auto rig = TemporalRig::create(*g, extent, extent);
    ASSERT_OK(rig);
    const TemporalFrame image = checker(extent, 0);

    auto up = make_upscaler(*g);
    ASSERT_NE(up, nullptr);
    float last = 0.0F;
    for (int i = 0; i < 8; ++i) {
        auto o = rig->frame(image, with(*up, static_cast<std::uint32_t>(i % 2)));
        ASSERT_OK(o) << "frame " << i;
        last = max_abs_diff(*o, image);
    }
    EXPECT_LE(last, k_exact) << "static image: history was resampled";

    auto control = make_upscaler(*g);
    ASSERT_NE(control, nullptr);
    TemporalFrame shifted = image;
    shifted.velocity.assign(rig->render_pixels(), glm::vec2(0.5F / 16.0F, 0.0F));
    for (int i = 0; i < 8; ++i) {
        auto o = rig->frame(shifted, with(*control, static_cast<std::uint32_t>(i % 2)));
        ASSERT_OK(o) << "control frame " << i;
        last = max_abs_diff(*o, image);
    }
    EXPECT_GT(last, 0.1F) << "control run unexpectedly stayed sharp";
}

// A2: a resize (new display extent) recreates the history and drops its
// validity, so the first frame at the new size is exactly the current frame;
// accumulation resumes on the next one. reset_history() does the same.
TEST(GpuTaa, ResizeResetsHistory) {
    QUAT_REQUIRE_GPU(g);
    auto up = make_upscaler(*g);
    ASSERT_NE(up, nullptr);

    constexpr VkExtent2D small{16, 16};
    auto rig_small = TemporalRig::create(*g, small, small);
    ASSERT_OK(rig_small);
    std::uint32_t frame = 0;
    for (; frame < 6; ++frame) {
        ASSERT_OK(rig_small->frame(checker(small, frame), with(*up, frame % 2)));
    }
    ASSERT_TRUE(up->history().valid());

    constexpr VkExtent2D big{24, 20};
    auto rig_big = TemporalRig::create(*g, big, big);
    ASSERT_OK(rig_big);
    const TemporalFrame first = checker(big, 0);
    auto o = rig_big->frame(first, with(*up, frame++ % 2));
    ASSERT_OK(o);
    EXPECT_EQ(up->history().extent().width, big.width);
    EXPECT_EQ(up->history().extent().height, big.height);
    EXPECT_LE(max_abs_diff(*o, first), k_exact) << "stale history blended after a resize";

    // History is live again at the new size: the flipped checker blends.
    auto next = rig_big->frame(checker(big, 1), with(*up, frame++ % 2));
    ASSERT_OK(next);
    EXPECT_NEAR(quat::test::grey(*next, 12, 10),
                from_y(glm::mix(to_y(k_hi), to_y(k_lo), current_weight(1))), 0.01F)
        << "history not accumulating after the resize";

    // An explicit reset (camera cut) behaves like a resize.
    up->reset_history();
    const TemporalFrame cut = checker(big, 0);
    auto after_cut = rig_big->frame(cut, with(*up, frame++ % 2));
    ASSERT_OK(after_cut);
    EXPECT_LE(max_abs_diff(*after_cut, cut), k_exact) << "history blended after a reset";
}

// Without history the output is the current frame.
TEST(GpuTaa, FirstFrameShowsTheCurrentFrame) {
    QUAT_REQUIRE_GPU(g);
    const std::vector<float> centres = resolve_uniform_frames(*g, {0.5F});
    ASSERT_EQ(centres.size(), 1U);
    EXPECT_NEAR(centres[0], 0.5F, k_exact);
}

// History matching the current frame resolves to itself.
TEST(GpuTaa, UniformFrameResolvesToItself) {
    QUAT_REQUIRE_GPU(g);
    const std::vector<float> centres = resolve_uniform_frames(*g, {0.5F, 0.5F});
    ASSERT_EQ(centres.size(), 2U);
    EXPECT_NEAR(centres[1], 0.5F, k_exact);
}

// Switching a flat region from 0.5 to 0.8: the neighbourhood clamp rejects the
// stale history, so it resolves to 0.8, not a ghosted blend.
TEST(GpuTaa, ClampRejectsStaleHistoryInFlatRegions) {
    QUAT_REQUIRE_GPU(g);
    const std::vector<float> centres = resolve_uniform_frames(*g, {0.5F, 0.5F, k_hi});
    ASSERT_EQ(centres.size(), 3U);
    EXPECT_NEAR(centres[2], k_hi, k_exact);
}

namespace {

constexpr VkExtent2D k_scene{32, 32};
constexpr float k_background = 0.05F;
constexpr float k_object = 2.0F;
constexpr int k_square = 6;

// A bright square (near) at column `x0` over a dark background (far); with
// `motion` the velocity target holds the square's motion of +1 pixel per
// frame, otherwise it is camera-only (zero: the camera is static), which is
// what depth-only reprojection amounted to.
TemporalFrame moving_square(int x0, bool motion) {
    TemporalFrame f;
    const std::size_t n = static_cast<std::size_t>(k_scene.width) * k_scene.height;
    f.color.assign(n, glm::vec4(k_background, k_background, k_background, 1.0F));
    f.depth.assign(n, 0.9F);
    f.velocity.assign(n, glm::vec2(0.0F));
    for (int y = 13; y < 13 + k_square; ++y) {
        for (int x = x0; x < x0 + k_square; ++x) {
            const std::size_t i = static_cast<std::size_t>(y) * k_scene.width +
                                  static_cast<std::size_t>(x);
            f.color[i] = glm::vec4(k_object, k_object, k_object, 1.0F);
            f.depth[i] = 0.3F;
            if (motion) f.velocity[i] = glm::vec2(1.0F / static_cast<float>(k_scene.width), 0.0F);
        }
    }
    return f;
}

// Mean error of the resolved square scene against the frame's true image, in
// tonemapped units (g / (1 + g)) so the bright object does not dominate.
float ghosting_error(const HostImage& out, const TemporalFrame& truth) {
    double sum = 0.0;
    for (std::uint32_t y = 0; y < k_scene.height; ++y) {
        for (std::uint32_t x = 0; x < k_scene.width; ++x) {
            const float got = quat::test::grey(out, x, y);
            const float want = truth.color[y * k_scene.width + x].r;
            sum += std::abs(to_y(got) - to_y(want));
        }
    }
    return static_cast<float>(sum / (k_scene.width * k_scene.height));
}

} // namespace

// R11: an object moving across a static background. Reprojecting its pixels
// with their own motion keeps the history aligned; camera-only reprojection
// (the pre-R11 depth path) smears the history behind and inside the object.
TEST(GpuTaa, VelocityReprojectionReducesGhostingOfAMovingObject) {
    QUAT_REQUIRE_GPU(g);
    auto rig = TemporalRig::create(*g, k_scene, k_scene);
    ASSERT_OK(rig);
    float error[2] = {0.0F, 0.0F};
    for (int mode = 0; mode < 2; ++mode) {
        const bool motion = mode == 1;
        auto up = make_upscaler(*g);
        ASSERT_NE(up, nullptr);
        constexpr int k_frames = 14;
        for (int i = 0; i < k_frames; ++i) {
            const TemporalFrame f = moving_square(4 + i, motion);
            auto o = rig->frame(f, with(*up, static_cast<std::uint32_t>(i % 2)));
            ASSERT_OK(o) << "frame " << i;
            if (i >= k_frames - 4) error[mode] += ghosting_error(*o, f) / 4.0F;
        }
    }
    RecordProperty("ghosting_depth_only", std::to_string(error[0]));
    RecordProperty("ghosting_velocity", std::to_string(error[1]));
    std::printf("[taa] mean ghosting error: camera-only %.4f, velocity %.4f\n",
                static_cast<double>(error[0]), static_cast<double>(error[1]));
    EXPECT_LT(error[1], error[0] * 0.5F)
        << "velocity " << error[1] << " vs camera-only " << error[0];
    EXPECT_LT(error[1], 0.01F) << "the moving object still ghosts with velocity";
}

// Issue #45: a non-finite reprojection (a NaN velocity, e.g. from a broken
// camera matrix) for one frame, then an Inf colour sample, must not leave
// NaN/Inf in the persistent history. The reprojection is rejected (and
// counted), the frame shows the current image, the Inf sample counts as black
// (counted), and every output stays finite and keeps resolving the uniform
// input away from the bad sample.
TEST(GpuTaa, NonFiniteInputsNeverReachTheHistory) {
    QUAT_REQUIRE_GPU(g);
    constexpr VkExtent2D extent{16, 16};
    auto rig = TemporalRig::create(*g, extent, extent);
    ASSERT_OK(rig);
    auto up = make_upscaler(*g);
    ASSERT_NE(up, nullptr);
    auto counters = r::NonFiniteCounters::create(g->device, g->allocator, 1);
    ASSERT_OK(counters);
    const VkDeviceAddress counter = counters->address(0, r::NonFiniteSource::taa_history);

    constexpr float k_grey = 0.5F;
    constexpr int k_frames = 12;
    constexpr int k_nan_velocity_frame = 3;
    constexpr int k_inf_color_frame = 6;
    constexpr std::uint32_t k_bad_x = 4;
    constexpr std::uint32_t k_bad_y = 4;
    for (int i = 0; i < k_frames; ++i) {
        TemporalFrame f = uniform(extent, k_grey);
        if (i == k_nan_velocity_frame) {
            f.velocity.assign(f.color.size(),
                              glm::vec2(std::numeric_limits<float>::quiet_NaN()));
        }
        if (i == k_inf_color_frame) {
            const float inf = std::numeric_limits<float>::infinity();
            f.color[k_bad_y * extent.width + k_bad_x] = glm::vec4(inf, inf, inf, 1.0F);
        }
        const auto slot = static_cast<std::uint32_t>(i % 2);
        auto o = rig->frame(f, [&up, slot, counter](rhi::RenderGraph& graph,
                                                     const r::UpscalerInputs& in) {
            r::UpscalerInputs counted = in;
            counted.non_finite_counter = counter;
            return up->add_to_graph(graph, slot, counted);
        });
        ASSERT_OK(o) << "frame " << i;
        // Per-frame delta (collect() reads and zeroes the counter): each bad
        // input must be rejected and counted on its own frame.
        const auto counts = counters->collect(0);
        const std::uint32_t rejected =
            counts[static_cast<std::size_t>(r::NonFiniteSource::taa_history)];
        if (i == k_nan_velocity_frame) {
            EXPECT_GE(rejected, 1U) << "the NaN reprojection was not counted";
        } else if (i == k_inf_color_frame) {
            EXPECT_GE(rejected, 1U) << "the Inf colour sample was not counted";
        } else {
            EXPECT_EQ(rejected, 0U) << "frame " << i << " has only finite inputs";
        }
        for (std::uint32_t y = 0; y < extent.height; ++y) {
            for (std::uint32_t x = 0; x < extent.width; ++x) {
                const glm::vec4 c = o->rgba16f(x, y);
                ASSERT_TRUE(std::isfinite(c.r) && std::isfinite(c.g) && std::isfinite(c.b))
                    << "frame " << i << " pixel " << x << "," << y;
                // From the Inf frame on, the bad pixel and its neighbours may
                // lag (it resolved black once); everything else stays exact.
                const bool near_bad =
                    i >= k_inf_color_frame &&
                    std::abs(static_cast<int>(x) - static_cast<int>(k_bad_x)) <= 1 &&
                    std::abs(static_cast<int>(y) - static_cast<int>(k_bad_y)) <= 1;
                if (!near_bad) {
                    ASSERT_NEAR(c.r, k_grey, k_exact)
                        << "frame " << i << " pixel " << x << "," << y;
                }
            }
        }
    }
}
