// A8: render settings reach the passes. A uniform HDR image is tonemapped
// with exposure settings (manual EV, auto key / compensation / clamps) and the
// RGBA8 result is compared with the CPU reference of tonemap.slang; the TAA
// settings (enabled, feedback) drive the temporal resolve.

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <expected>

#include <glm/glm.hpp>
#include <gtest/gtest.h>
#include <vulkan/vulkan.h>

#include "engine/renderer/exposure_pass.hpp"
#include "engine/renderer/render_settings.hpp"
#include "engine/renderer/taa_pass.hpp"
#include "engine/renderer/upscaler.hpp"
#include "engine/renderer/tonemap_pass.hpp"
#include "engine/rhi/device.hpp"
#include "engine/rhi/render_graph.hpp"
#include "render/pbr_reference.hpp"
#include "render/temporal_rig.hpp"
#include "support/expect_ok.hpp"
#include "support/gpu_commands.hpp"
#include "support/gpu_context.hpp"
#include "support/render_readback.hpp"

namespace {

namespace r = engine::renderer;
namespace rhi = engine::rhi;
using engine::core::Error;
using quat::test::HostImage;

constexpr VkExtent2D k_extent{32, 32};
constexpr VkFormat   k_hdr_format = VK_FORMAT_R16G16B16A16_SFLOAT;
constexpr VkFormat   k_ldr_format = VK_FORMAT_R8G8B8A8_UNORM;
constexpr VkImageLayout k_ro = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;

// A persistent HDR image cleared to grey `value` at the start of each submit.
struct UniformHdr {
    rhi::GpuImage  image;
    rhi::ImageView view;
    float          value = 0.0F;

    [[nodiscard]] static std::expected<UniformHdr, Error> create(quat::test::GpuContext& g,
                                                                 float value) {
        UniformHdr out;
        out.value = value;
        auto img = g.allocator.create_image(
            k_hdr_format, k_extent, VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT);
        if (!img) return std::unexpected(img.error());
        auto view = rhi::create_image_view(g.device.handle(), img->handle(), k_hdr_format);
        if (!view) return std::unexpected(view.error());
        out.image = std::move(*img);
        out.view = std::move(*view);
        return out;
    }

    void clear(VkCommandBuffer cmd) const {
        const VkImage img = image.handle();
        quat::test::image_barrier(cmd, img, VK_IMAGE_ASPECT_COLOR_BIT, VK_IMAGE_LAYOUT_UNDEFINED,
                                  VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
                                  VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT, 0,
                                  VK_PIPELINE_STAGE_2_CLEAR_BIT, VK_ACCESS_2_TRANSFER_WRITE_BIT);
        VkClearColorValue c{};
        c.float32[0] = c.float32[1] = c.float32[2] = value;
        c.float32[3] = 1.0F;
        const VkImageSubresourceRange range{VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
        vkCmdClearColorImage(cmd, img, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, &c, 1, &range);
        quat::test::image_barrier(cmd, img, VK_IMAGE_ASPECT_COLOR_BIT,
                                  VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, k_ro,
                                  VK_PIPELINE_STAGE_2_CLEAR_BIT, VK_ACCESS_2_TRANSFER_WRITE_BIT,
                                  VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT,
                                  VK_ACCESS_2_SHADER_SAMPLED_READ_BIT);
    }

    [[nodiscard]] rhi::ResourceHandle import(rhi::RenderGraph& graph) const {
        return graph.import_image("test_hdr", image.handle(), view.handle(), k_hdr_format,
                                  k_extent, k_ro, k_ro);
    }
};

// sRGB(ACES(hdr * exposure)) of one channel, as 0..255.
int reference(float hdr, float exposure) {
    const float mapped = quat::test::pbr_ref::aces(glm::vec3(hdr * exposure)).x;
    return static_cast<int>(std::round(quat::test::pbr_ref::linear_to_srgb(mapped) * 255.0F));
}

// Tonemaps `hdr` with `settings`; with `exposure` the auto-exposure pass
// measures the image first (dt far beyond the time constant: fully adapted).
std::expected<HostImage, Error> tonemap(quat::test::GpuContext& g, const UniformHdr& hdr,
                                        const r::ExposureSettings& settings,
                                        r::ExposurePass* exposure) {
    auto pass = r::TonemapPass::create(g.device, g.allocator, g.cache, g.shader_dir, k_ldr_format);
    if (!pass) return std::unexpected(pass.error());
    rhi::TransientImagePool pool(g.device, g.allocator);
    rhi::RenderGraph graph(pool);
    const rhi::ResourceHandle in = hdr.import(graph);
    VkDeviceAddress exposure_buffer = 0;
    if (exposure != nullptr) {
        if (auto a = exposure->add_to_graph(graph, in, k_extent, 1000.0F, settings.adaptation_s);
            !a) {
            return std::unexpected(a.error());
        }
        exposure_buffer = exposure->exposure_buffer_address();
    }
    const rhi::ResourceHandle out = graph.create_transient_image(
        "ldr", k_ldr_format, k_extent,
        VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT);
    if (auto a = pass->add_to_graph(graph, in, out, k_extent, exposure_buffer, settings); !a) {
        return std::unexpected(a.error());
    }
    return quat::test::render_to_host(g, graph, out, k_extent, 4,
                                      [&](VkCommandBuffer cmd) { hdr.clear(cmd); });
}

TEST(GpuRenderSettings, ManualExposureCompensationScalesTheImage) {
    QUAT_REQUIRE_GPU(g);
    auto hdr = UniformHdr::create(*g, 0.25F);
    ASSERT_OK(hdr);
    r::ExposureSettings manual;
    manual.auto_exposure = false;
    int previous = -1;
    for (const float ev : {-1.0F, 0.0F, 1.0F, 2.0F}) {
        manual.compensation_ev = ev;
        auto ldr = tonemap(*g, *hdr, manual, nullptr);
        ASSERT_OK(ldr);
        const glm::ivec4 px = ldr->rgba8(5, 7);
        const int expected = reference(0.25F, std::exp2(ev));
        EXPECT_NEAR(px.r, expected, 1) << "EV " << ev;
        EXPECT_GT(px.r, previous) << "EV " << ev << " did not brighten the image";
        previous = px.r;
    }
}

TEST(GpuRenderSettings, AutoExposureAppliesKeyClampsAndCompensation) {
    QUAT_REQUIRE_GPU(g);
    auto hdr = UniformHdr::create(*g, 0.5F);
    ASSERT_OK(hdr);
    auto exposure = r::ExposurePass::create(g->device, g->allocator, g->cache, g->shader_dir);
    ASSERT_OK(exposure);

    r::ExposureSettings s; // auto
    s.key = 0.3F;
    s.compensation_ev = 1.0F;
    auto ldr = tonemap(*g, *hdr, s, &*exposure);
    ASSERT_OK(ldr);
    const float adapted = exposure->adapted_luminance();
    ASSERT_GT(adapted, 0.1F);
    const float e = std::clamp(s.key / adapted, s.min_exposure, s.max_exposure) * 2.0F;
    EXPECT_NEAR(ldr->rgba8(3, 3).r, reference(0.5F, e), 2);

    // The max clamp caps the auto exposure (before compensation).
    s.compensation_ev = 0.0F;
    s.max_exposure = 0.1F;
    ldr = tonemap(*g, *hdr, s, &*exposure);
    ASSERT_OK(ldr);
    EXPECT_NEAR(ldr->rgba8(3, 3).r, reference(0.5F, 0.1F), 2);
}

// TAA settings reach the resolve (R11 TAAU, mapped by apply_taa_settings):
// with TAA off it shows the current frame even where the history (black,
// inside the neighbourhood clip) would otherwise dominate; feedback is the
// history weight (0: current frame only).
TEST(GpuRenderSettings, TaaSettingsDriveTheResolve) {
    QUAT_REQUIRE_GPU(g);
    auto rig = quat::test::TemporalRig::create(*g, k_extent, k_extent);
    ASSERT_OK(rig);
    auto up = r::TaaUpscaler::create(g->device, g->allocator, g->cache, g->shader_dir, 1);
    ASSERT_OK(up);
    // Left half `grey`, right half black: the edge column's neighbourhood
    // spans [0, grey], so the clip keeps a black history there.
    const auto run = [&](float grey, const r::TaaSettings& settings) -> std::expected<float, Error> {
        quat::test::TemporalFrame f;
        for (std::uint32_t y = 0; y < k_extent.height; ++y) {
            for (std::uint32_t x = 0; x < k_extent.width; ++x) {
                const float v = x < k_extent.width / 2 ? grey : 0.0F;
                f.color.emplace_back(v, v, v, 1.0F);
            }
        }
        auto out = rig->frame(f, [&](rhi::RenderGraph& graph, r::UpscalerInputs in) {
            r::apply_taa_settings(in, settings);
            return (*up)->add_to_graph(graph, 0, in);
        });
        if (!out) return std::unexpected(out.error());
        return quat::test::grey(*out, k_extent.width / 2 - 1, 16); // the edge column
    };
    r::TaaSettings on;
    for (int i = 0; i < 12; ++i) ASSERT_OK(run(0.0F, on)); // settle a black history
    auto blended = run(1.0F, on);
    ASSERT_OK(blended);
    EXPECT_LT(*blended, 0.5F) << "with TAA on the black history dominates (feedback 0.9)";

    for (int i = 0; i < 12; ++i) ASSERT_OK(run(0.0F, on));
    r::TaaSettings off;
    off.enabled = false;
    auto through = run(1.0F, off);
    ASSERT_OK(through);
    EXPECT_NEAR(*through, 1.0F, 3e-3F) << "TAA off must pass the current frame through";

    for (int i = 0; i < 12; ++i) ASSERT_OK(run(0.0F, on));
    r::TaaSettings no_feedback;
    no_feedback.feedback = 0.0F;
    auto current = run(1.0F, no_feedback);
    ASSERT_OK(current);
    EXPECT_NEAR(*current, 1.0F, 3e-3F) << "feedback 0 keeps no history";
}

} // namespace
