// The Upscaler interface's TAAU backend (R11) on a static scene: rendering at
// 50% per axis with the Halton jitter, the accumulated display-resolution
// output converges to a supersampled reference about as closely as native
// TAA does, and clearly beats upscaling a single frame bilinearly.
//
// The scene is an analytic image (smooth shading, a fine sinusoid and a
// hard-edged bright disc) evaluated exactly where each jittered render sample
// lies, so the test isolates the resolve from rasterisation. Errors are
// measured in the resolve's tonemapped space (y = g / (1 + g), exposure 1).

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <memory>
#include <string>
#include <vector>

#include <glm/glm.hpp>
#include <gtest/gtest.h>
#include <vulkan/vulkan.h>

#include "engine/renderer/jitter.hpp"
#include "engine/renderer/upscaler.hpp"
#include "render/temporal_rig.hpp"
#include "support/expect_ok.hpp"
#include "support/gpu_context.hpp"

namespace {

namespace r = engine::renderer;
namespace rhi = engine::rhi;
using quat::test::HostImage;
using quat::test::TemporalFrame;
using quat::test::TemporalRig;

constexpr VkExtent2D k_display{64, 64};

float to_y(float g) { return g / (1.0F + g); }

float scene(glm::vec2 uv) {
    float v = 0.25F + 0.5F * uv.x * uv.y +
              0.15F * std::sin(uv.x * 23.0F) * std::cos(uv.y * 17.0F);
    if (glm::length(uv - glm::vec2(0.55F, 0.45F)) < 0.22F) v = 1.2F;
    return v;
}

// The render-extent frame for `jitter`: each sample holds the scene at its
// unjittered position (i + 0.5 - jitter) / extent (jitter.hpp).
TemporalFrame render_frame(VkExtent2D extent, const r::ViewJitter& jitter) {
    TemporalFrame f;
    f.jitter = jitter;
    f.color.resize(static_cast<std::size_t>(extent.width) * extent.height);
    for (std::uint32_t y = 0; y < extent.height; ++y) {
        for (std::uint32_t x = 0; x < extent.width; ++x) {
            const glm::vec2 uv((static_cast<float>(x) + 0.5F - jitter.pixels.x) /
                                   static_cast<float>(extent.width),
                               (static_cast<float>(y) + 0.5F - jitter.pixels.y) /
                                   static_cast<float>(extent.height));
            const float v = scene(uv);
            f.color[static_cast<std::size_t>(y) * extent.width + x] = glm::vec4(v, v, v, 1.0F);
        }
    }
    return f;
}

// Box-filtered display-resolution reference (8 x 8 samples per pixel,
// averaged in resolve space).
std::vector<float> reference() {
    constexpr int k_sub = 8;
    std::vector<float> out(static_cast<std::size_t>(k_display.width) * k_display.height);
    for (std::uint32_t y = 0; y < k_display.height; ++y) {
        for (std::uint32_t x = 0; x < k_display.width; ++x) {
            float sum = 0.0F;
            for (int sy = 0; sy < k_sub; ++sy) {
                for (int sx = 0; sx < k_sub; ++sx) {
                    const glm::vec2 uv(
                        (static_cast<float>(x) + (static_cast<float>(sx) + 0.5F) / k_sub) /
                            static_cast<float>(k_display.width),
                        (static_cast<float>(y) + (static_cast<float>(sy) + 0.5F) / k_sub) /
                            static_cast<float>(k_display.height));
                    sum += to_y(scene(uv));
                }
            }
            out[static_cast<std::size_t>(y) * k_display.width + x] = sum / (k_sub * k_sub);
        }
    }
    return out;
}

// Mean |y(output) - y(reference)| over the display.
float mean_error(const std::vector<float>& got_y, const std::vector<float>& ref_y) {
    double sum = 0.0;
    for (std::size_t i = 0; i < ref_y.size(); ++i) sum += std::abs(got_y[i] - ref_y[i]);
    return static_cast<float>(sum / static_cast<double>(ref_y.size()));
}

std::vector<float> output_y(const HostImage& img) {
    std::vector<float> out;
    for (std::uint32_t y = 0; y < img.extent.height; ++y) {
        for (std::uint32_t x = 0; x < img.extent.width; ++x) {
            out.push_back(to_y(quat::test::grey(img, x, y)));
        }
    }
    return out;
}

// Runs `frames` jittered frames at `render` through a fresh TAAU and returns
// the last output in resolve space (empty on failure).
std::vector<float> accumulate(quat::test::GpuContext& g, VkExtent2D render, int frames) {
    auto rig = TemporalRig::create(g, render, k_display);
    auto up = r::create_upscaler(r::UpscalerBackend::taau, g.device, g.allocator, g.cache,
                                 g.shader_dir, 2);
    if (!rig || !up) return {};
    const std::uint32_t phases = r::jitter_phase_count(render, k_display);
    std::vector<float> last;
    for (int i = 0; i < frames; ++i) {
        const TemporalFrame f =
            render_frame(render, r::halton_jitter(static_cast<std::uint64_t>(i), render, phases));
        const auto slot = static_cast<std::uint32_t>(i % 2);
        auto out = rig->frame(f, [&](rhi::RenderGraph& graph, const r::UpscalerInputs& in) {
            return (*up)->add_to_graph(graph, slot, in);
        });
        if (!out) return {};
        last = output_y(*out);
    }
    return last;
}

// A single unjittered render-extent frame upscaled bilinearly (the baseline a
// temporal upscaler must beat), in resolve space.
std::vector<float> bilinear(VkExtent2D render) {
    const TemporalFrame f = render_frame(render, r::ViewJitter{});
    const auto at = [&](int x, int y) {
        x = std::clamp(x, 0, static_cast<int>(render.width) - 1);
        y = std::clamp(y, 0, static_cast<int>(render.height) - 1);
        return to_y(f.color[static_cast<std::size_t>(y) * render.width + static_cast<std::size_t>(x)].r);
    };
    std::vector<float> out;
    for (std::uint32_t y = 0; y < k_display.height; ++y) {
        for (std::uint32_t x = 0; x < k_display.width; ++x) {
            const float sx = (static_cast<float>(x) + 0.5F) * static_cast<float>(render.width) /
                                 static_cast<float>(k_display.width) - 0.5F;
            const float sy = (static_cast<float>(y) + 0.5F) * static_cast<float>(render.height) /
                                 static_cast<float>(k_display.height) - 0.5F;
            const int x0 = static_cast<int>(std::floor(sx));
            const int y0 = static_cast<int>(std::floor(sy));
            const float fx = sx - static_cast<float>(x0);
            const float fy = sy - static_cast<float>(y0);
            out.push_back(glm::mix(glm::mix(at(x0, y0), at(x0 + 1, y0), fx),
                                   glm::mix(at(x0, y0 + 1), at(x0 + 1, y0 + 1), fx), fy));
        }
    }
    return out;
}

} // namespace

TEST(GpuUpscaler, HalfResolutionConvergesTowardNativeOnAStaticScene) {
    QUAT_REQUIRE_GPU(g);
    const std::vector<float> ref = reference();
    constexpr VkExtent2D k_half{32, 32};

    // Three jitter cycles at 50% (32 phases each), one at native (8 phases x 4).
    const std::vector<float> taau = accumulate(*g, k_half, 96);
    ASSERT_EQ(taau.size(), ref.size());
    const std::vector<float> native = accumulate(*g, k_display, 32);
    ASSERT_EQ(native.size(), ref.size());

    const float err_taau = mean_error(taau, ref);
    const float err_native = mean_error(native, ref);
    const float err_bilinear = mean_error(bilinear(k_half), ref);
    RecordProperty("error_taau_50", std::to_string(err_taau));
    RecordProperty("error_native_taa", std::to_string(err_native));
    RecordProperty("error_bilinear_50", std::to_string(err_bilinear));
    std::printf("[upscaler] mean |dy| vs supersampled: taau 50%% %.4f, native taa %.4f, "
                "bilinear 50%% %.4f\n",
                static_cast<double>(err_taau), static_cast<double>(err_native),
                static_cast<double>(err_bilinear));
    EXPECT_LT(err_taau, err_bilinear * 0.5F) << "TAAU did not beat a bilinear upscale";
    EXPECT_LT(err_taau, err_native + 0.002F) << "TAAU too far from native TAA";
    EXPECT_LT(err_taau, 0.004F);
}

// Settings pick the render extent; a missing backend is reported, not faked.
TEST(GpuUpscaler, ModesSelectTheRenderExtentAndFsrReportsUnavailable) {
    QUAT_REQUIRE_GPU(g);
    r::UpscalerSettings s;
    s.mode = static_cast<int>(r::UpscalerMode::performance);
    EXPECT_EQ(r::upscaler_render_extent(k_display, s).width, 32U);
    auto fsr = r::create_upscaler(r::UpscalerBackend::fsr2, g->device, g->allocator, g->cache,
                                  g->shader_dir, 2);
    EXPECT_FALSE(fsr.has_value());
    auto taau = r::create_upscaler(r::UpscalerBackend::taau, g->device, g->allocator, g->cache,
                                   g->shader_dir, 2);
    ASSERT_OK(taau);
    EXPECT_EQ((*taau)->backend(), r::UpscalerBackend::taau);
}
