// Auto-exposure: the luminance histogram measures uniform scenes, and the
// adapted value moves between frames at the configured rate (audit S1 A11).
//
// For the latter, frames alternate between two ExposurePass instances (the
// engine's two frame-in-flight slots) sharing one ExposureState. The adapted
// luminance must follow a single exponential with the configured time
// constant, advanced by each frame's dt — with per-slot state each slot adapted
// on its own (half the rate) and consecutive frames alternated between the two
// values.

#include <cmath>
#include <cstdint>
#include <expected>
#include <memory>
#include <string>

#include <gtest/gtest.h>
#include <vulkan/vulkan.h>

#include "engine/renderer/exposure_pass.hpp"
#include "engine/rhi/device.hpp"
#include "engine/rhi/gpu_allocator.hpp"
#include "engine/rhi/render_graph.hpp"
#include "support/expect_ok.hpp"
#include "support/gpu_commands.hpp"
#include "support/gpu_context.hpp"

namespace {

namespace r = engine::renderer;
namespace rhi = engine::rhi;
using engine::core::Error;

constexpr VkFormat   k_hdr_format = VK_FORMAT_R16G16B16A16_SFLOAT;
constexpr VkExtent2D k_extent{32, 32};

[[nodiscard]] std::unexpected<Error> fail(std::string what) {
    return std::unexpected(Error{std::move(what)});
}

// A uniform HDR image the exposure pass measures.
class HdrImage {
public:
    [[nodiscard]] static std::expected<HdrImage, Error> create(quat::test::GpuContext& g) {
        HdrImage out;
        out.g_ = &g;
        auto img = g.allocator.create_image(
            k_hdr_format, k_extent, VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT);
        if (!img) return std::unexpected(img.error());
        auto view = rhi::create_image_view(g.device.handle(), img->handle(), k_hdr_format);
        if (!view) return std::unexpected(view.error());
        out.image_ = std::move(*img);
        out.view_ = std::move(*view);
        return out;
    }

    // Fill with grey `value`, run one exposure frame advancing by `dt` and
    // return the shared adapted luminance afterwards.
    [[nodiscard]] std::expected<float, Error> frame(r::ExposurePass& pass, float value, float dt) {
        const VkImage image = image_.handle();
        const bool filled = quat::test::submit_blocking(g_->device, [&](VkCommandBuffer cmd) {
            quat::test::image_barrier(cmd, image, VK_IMAGE_ASPECT_COLOR_BIT,
                                      VK_IMAGE_LAYOUT_UNDEFINED,
                                      VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
                                      VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT, 0,
                                      VK_PIPELINE_STAGE_2_CLEAR_BIT,
                                      VK_ACCESS_2_TRANSFER_WRITE_BIT);
            VkClearColorValue c{};
            c.float32[0] = c.float32[1] = c.float32[2] = value;
            c.float32[3] = 1.0F;
            const VkImageSubresourceRange range{VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
            vkCmdClearColorImage(cmd, image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, &c, 1, &range);
            quat::test::image_barrier(cmd, image, VK_IMAGE_ASPECT_COLOR_BIT,
                                      VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
                                      VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
                                      VK_PIPELINE_STAGE_2_CLEAR_BIT,
                                      VK_ACCESS_2_TRANSFER_WRITE_BIT,
                                      VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT,
                                      VK_ACCESS_2_SHADER_SAMPLED_READ_BIT);
        });
        if (!filled) return fail("exposure test: fill failed");

        rhi::TransientImagePool pool(g_->device, g_->allocator);
        rhi::RenderGraph graph(pool);
        constexpr VkImageLayout ro = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
        const rhi::ResourceHandle hdr = graph.import_image("test_hdr", image, view_.handle(),
                                                           k_hdr_format, k_extent, ro, ro);
        r::ExposureMetering metering;
        metering.pre_exposure = pre_exposure_;
        if (auto a = pass.add_to_graph(graph, hdr, k_extent, dt, metering); !a) {
            return std::unexpected(a.error());
        }
        if (auto c = graph.compile(); !c) return std::unexpected(c.error());
        const bool ran = quat::test::submit_blocking(
            g_->device, [&](VkCommandBuffer cmd) { graph.execute(cmd); });
        if (!ran) return fail("exposure test: frame submit failed");
        return pass.adapted_luminance();
    }

    // The pre-exposure the next frames' images are measured with (the image
    // holds luminance x pre).
    void set_pre_exposure(float pre) { pre_exposure_ = pre; }

private:
    quat::test::GpuContext* g_ = nullptr;
    float                   pre_exposure_ = 1.0F;
    rhi::GpuImage           image_;
    rhi::ImageView          view_;
};

} // namespace

TEST(GpuExposure, SharedStateAdaptsWithConfiguredTimeConstant) {
    QUAT_REQUIRE_GPU(g);
    auto hdr = HdrImage::create(*g);
    ASSERT_OK(hdr);
    constexpr float k_dim = 0.25F;
    constexpr float k_bright = 2.0F;
    constexpr float k_instant = 1000.0F; // dt >> tau: fully adapted in one frame

    // Reference: what the histogram measures for each scene (fully adapted).
    auto reference = r::ExposurePass::create(g->device, g->allocator, g->cache, g->shader_dir);
    ASSERT_OK(reference);
    auto l_dim = hdr->frame(*reference, k_dim, k_instant);
    ASSERT_OK(l_dim);
    auto l_bright = hdr->frame(*reference, k_bright, k_instant);
    ASSERT_OK(l_bright);
    ASSERT_GT(*l_bright, *l_dim * 2.0F) << "histogram does not separate the scenes";

    // Two slots, one shared state.
    auto state = r::ExposureState::create(g->device, g->allocator);
    ASSERT_OK(state);
    auto slot_a =
        r::ExposurePass::create(g->device, g->allocator, g->cache, g->shader_dir, *state);
    auto slot_b =
        r::ExposurePass::create(g->device, g->allocator, g->cache, g->shader_dir, *state);
    ASSERT_OK(slot_a);
    ASSERT_OK(slot_b);
    EXPECT_EQ(slot_a->exposure_buffer_address(), slot_b->exposure_buffer_address());
    r::ExposurePass* slots[2] = {&*slot_a, &*slot_b};

    // Adapted to the dim scene, then the scene brightens: after n frames of dt
    // the adapted log luminance is log l_bright + (log l_dim - log l_bright) *
    // exp(-n dt / tau) (R22.2.1 adapts in the log domain, as the eye's
    // response is logarithmic), whichever slot recorded each frame.
    auto seeded = hdr->frame(*slots[0], k_dim, k_instant);
    ASSERT_OK(seeded);
    EXPECT_NEAR(*seeded, *l_dim, *l_dim * 1e-3F);

    constexpr float k_dt = 0.1F;
    for (int n = 1; n <= 10; ++n) {
        auto adapted = hdr->frame(*slots[n % 2], k_bright, k_dt);
        ASSERT_OK(adapted) << "frame " << n;
        const float expected = std::exp2(
            std::log2(*l_bright) + (std::log2(*l_dim) - std::log2(*l_bright)) *
                                       std::exp(-static_cast<float>(n) * k_dt /
                                                r::ExposurePass::k_adaptation_tau));
        EXPECT_NEAR(*adapted, expected, expected * 0.01F) << "frame " << n;
        EXPECT_FLOAT_EQ(slot_a->adapted_luminance(), slot_b->adapted_luminance());
    }
}

// A uniform grey 0.25 scene (luminance 0.25), fully adapted in one frame: the
// histogram measures it and the adapted luminance tracks it.
TEST(GpuExposure, AdaptsToADimScenesLuminance) {
    QUAT_REQUIRE_GPU(g);
    auto hdr = HdrImage::create(*g);
    ASSERT_OK(hdr);
    auto pass = r::ExposurePass::create(g->device, g->allocator, g->cache, g->shader_dir);
    ASSERT_OK(pass);
    auto dim = hdr->frame(*pass, 0.25F, 1000.0F);
    ASSERT_OK(dim);
    EXPECT_GE(*dim, 0.15F);
    EXPECT_LE(*dim, 0.45F);
}

// A bright scene (2.0) after the dim one pulls the adapted luminance well up.
TEST(GpuExposure, BrightSceneRaisesTheAdaptedLuminance) {
    QUAT_REQUIRE_GPU(g);
    auto hdr = HdrImage::create(*g);
    ASSERT_OK(hdr);
    auto pass = r::ExposurePass::create(g->device, g->allocator, g->cache, g->shader_dir);
    ASSERT_OK(pass);
    auto dim = hdr->frame(*pass, 0.25F, 1000.0F);
    ASSERT_OK(dim);
    auto bright = hdr->frame(*pass, 2.0F, 1000.0F);
    ASSERT_OK(bright);
    EXPECT_GT(*bright, *dim + 0.5F);
}
