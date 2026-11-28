// Auto-exposure metering and calibration on a real device (R22.2.1, #142):
//   - centre-weighted metering exposes a back-lit subject (a dim centre in
//     front of a bright sky) brighter than even metering, and the GPU
//     histogram matches the CPU mirror (photometry::metering_weight +
//     metered_log2);
//   - calibration: run the engine's loop (pre-exposed HDR, exposure,
//     ACES tonemap) on a daylight scene (a grey card under 100 klux, EV100
//     ~15, under a clear sky) and on a moonless night (the card under the
//     1 mlux night-sky floor, under the night dome). The displayed mean
//     luma stays in a band for each, so neither end can drift again:
//     daylight 110..150 of 255 (mid-grey lands at ~18 %, the photographic
//     calibration), night 4..40 (dimmer, Krawczyk et al. 2005).

#include <cmath>
#include <cstdint>
#include <cstring>
#include <expected>
#include <span>
#include <string>
#include <vector>

#include <glm/glm.hpp>
#include <glm/gtc/packing.hpp>
#include <gtest/gtest.h>
#include <vulkan/vulkan.h>

#include "engine/renderer/exposure_pass.hpp"
#include "engine/renderer/photometry.hpp"
#include "engine/renderer/render_settings.hpp"
#include "engine/renderer/tonemap_pass.hpp"
#include "engine/rhi/device.hpp"
#include "engine/rhi/gpu_allocator.hpp"
#include "engine/rhi/render_graph.hpp"
#include "support/expect_ok.hpp"
#include "support/gpu_commands.hpp"
#include "support/gpu_context.hpp"
#include "support/render_readback.hpp"

namespace {

namespace r = engine::renderer;
namespace rhi = engine::rhi;
namespace ph = engine::renderer::photometry;
using engine::core::Error;

constexpr VkExtent2D    k_extent{64, 64};
constexpr VkFormat      k_hdr_format = VK_FORMAT_R16G16B16A16_SFLOAT;
constexpr VkFormat      k_ldr_format = VK_FORMAT_R8G8B8A8_UNORM;
constexpr VkImageLayout k_ro = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;

// True luminance (HDR units) per pixel.
using Scene = std::vector<float>;

// Luminance of a 0.18 grey card under `lux` (HDR units).
[[nodiscard]] float grey_card(float lux) {
    return 0.18F * lux / 3.14159265F / ph::k_nits_per_unit;
}

// A two-region scene: `inner` where pred(u, v), `outer` elsewhere.
template <typename Pred>
[[nodiscard]] Scene two_regions(float inner, float outer, Pred pred) {
    Scene s;
    for (std::uint32_t y = 0; y < k_extent.height; ++y) {
        for (std::uint32_t x = 0; x < k_extent.width; ++x) {
            const float u = (static_cast<float>(x) + 0.5F) / static_cast<float>(k_extent.width);
            const float v = (static_cast<float>(y) + 0.5F) / static_cast<float>(k_extent.height);
            s.push_back(pred(u, v) ? inner : outer);
        }
    }
    return s;
}

// An RGBA16F image the tests fill with a pre-exposed scene.
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

    // Uploads scene * pre as grey half floats (RGBA16F rounding, as the
    // engine's HDR target) and leaves the image in SHADER_READ_ONLY.
    [[nodiscard]] std::expected<void, Error> fill(const Scene& scene, float pre) {
        std::vector<std::uint16_t> px;
        px.reserve(scene.size() * 4);
        for (const float l : scene) {
            const std::uint16_t h = glm::packHalf1x16(l * pre);
            px.insert(px.end(), {h, h, h, glm::packHalf1x16(1.0F)});
        }
        auto staging = g_->allocator.create_staging_buffer(px.size() * sizeof(std::uint16_t));
        if (!staging) return std::unexpected(staging.error());
        std::memcpy(staging->mapped(), px.data(), px.size() * sizeof(std::uint16_t));
        const VkImage image = image_.handle();
        const VkBuffer src = staging->handle();
        const bool ok = quat::test::submit_blocking(g_->device, [&](VkCommandBuffer cmd) {
            quat::test::image_barrier(cmd, image, VK_IMAGE_ASPECT_COLOR_BIT,
                                      VK_IMAGE_LAYOUT_UNDEFINED,
                                      VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
                                      VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT, 0,
                                      VK_PIPELINE_STAGE_2_COPY_BIT,
                                      VK_ACCESS_2_TRANSFER_WRITE_BIT);
            VkBufferImageCopy region{};
            region.imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
            region.imageExtent = {k_extent.width, k_extent.height, 1};
            vkCmdCopyBufferToImage(cmd, src, image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1,
                                   &region);
            quat::test::image_barrier(cmd, image, VK_IMAGE_ASPECT_COLOR_BIT,
                                      VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, k_ro,
                                      VK_PIPELINE_STAGE_2_COPY_BIT,
                                      VK_ACCESS_2_TRANSFER_WRITE_BIT,
                                      VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT,
                                      VK_ACCESS_2_SHADER_SAMPLED_READ_BIT);
        });
        if (!ok) return std::unexpected(Error{"metering test: upload failed"});
        return {};
    }

    [[nodiscard]] rhi::ResourceHandle import(rhi::RenderGraph& graph) const {
        return graph.import_image("test_hdr", image_.handle(), view_.handle(), k_hdr_format,
                                  k_extent, k_ro, k_ro);
    }

private:
    quat::test::GpuContext* g_ = nullptr;
    rhi::GpuImage           image_;
    rhi::ImageView          view_;
};

// One exposure frame over the image's current contents.
[[nodiscard]] std::expected<void, Error> meter(quat::test::GpuContext& g, const HdrImage& hdr,
                                               r::ExposurePass& pass, float dt,
                                               const r::ExposureMetering& metering) {
    rhi::TransientImagePool pool(g.device, g.allocator);
    rhi::RenderGraph graph(pool);
    const rhi::ResourceHandle in = hdr.import(graph);
    if (auto a = pass.add_to_graph(graph, in, k_extent, dt, metering); !a) {
        return std::unexpected(a.error());
    }
    if (auto c = graph.compile(); !c) return std::unexpected(c.error());
    if (!quat::test::submit_blocking(g.device, [&](VkCommandBuffer cmd) { graph.execute(cmd); })) {
        return std::unexpected(Error{"metering test: submit failed"});
    }
    return {};
}

// The CPU mirror: weighted histogram of the (half-rounded) scene, metered.
[[nodiscard]] float cpu_metered_log2(const Scene& scene, float center_weight) {
    std::array<std::uint32_t, ph::k_histogram_bins> bins{};
    for (std::uint32_t y = 0; y < k_extent.height; ++y) {
        for (std::uint32_t x = 0; x < k_extent.width; ++x) {
            const float u = (static_cast<float>(x) + 0.5F) / static_cast<float>(k_extent.width);
            const float v = (static_cast<float>(y) + 0.5F) / static_cast<float>(k_extent.height);
            const float l = glm::unpackHalf1x16(glm::packHalf1x16(scene[y * k_extent.width + x]));
            bins[ph::histogram_bin(l)] += static_cast<std::uint32_t>(
                std::round(ph::metering_weight(u, v, center_weight) * ph::k_metering_weight_one));
        }
    }
    return ph::metered_log2(bins);
}

// Mean sRGB luma (0..255) of the engine's loop on `scene` after `frames`
// frames of 1/30 s from a fresh state: pre-exposure = previous exposure.
[[nodiscard]] std::expected<float, Error> displayed_mean_luma(quat::test::GpuContext& g,
                                                               const Scene& scene, int frames) {
    auto hdr = HdrImage::create(g);
    if (!hdr) return std::unexpected(hdr.error());
    auto pass = r::ExposurePass::create(g.device, g.allocator, g.cache, g.shader_dir);
    if (!pass) return std::unexpected(pass.error());
    const r::ExposureSettings settings; // engine defaults
    float pre = 1.0F;
    for (int f = 0; f < frames; ++f) {
        if (auto ok = hdr->fill(scene, pre); !ok) return std::unexpected(ok.error());
        r::ExposureMetering m = r::ExposureMetering::from(settings, pre);
        m.reset = f == 0;
        if (auto ok = meter(g, *hdr, *pass, 1.0F / 30.0F, m); !ok) return std::unexpected(ok.error());
        pre = pass->exposure() * std::exp2(settings.compensation_ev);
    }
    // Tonemap the last frame's image (pre-exposed with the exposure it was
    // rendered with) exactly as the frame graph does.
    if (auto ok = hdr->fill(scene, pre); !ok) return std::unexpected(ok.error());
    auto tone = r::TonemapPass::create(g.device, g.allocator, g.cache, g.shader_dir, k_ldr_format);
    if (!tone) return std::unexpected(tone.error());
    rhi::TransientImagePool pool(g.device, g.allocator);
    rhi::RenderGraph graph(pool);
    const rhi::ResourceHandle in = hdr->import(graph);
    const rhi::ResourceHandle out = graph.create_transient_image(
        "ldr", k_ldr_format, k_extent,
        VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT);
    r::TonemapLook look;
    look.pre_exposure = pre;
    look.film.vignette_natural = 0.0F; // measure the exposure, not the lens
    look.film.vignette_intensity = 0.0F;
    look.film.grain_intensity = 0.0F;
    if (auto a = tone->add_to_graph(graph, in, out, k_extent, pass->exposure_buffer_address(),
                                    settings, look);
        !a) {
        return std::unexpected(a.error());
    }
    auto img = quat::test::render_to_host(g, graph, out, k_extent, 4);
    if (!img) return std::unexpected(img.error());
    double sum = 0.0;
    for (std::uint32_t y = 0; y < k_extent.height; ++y) {
        for (std::uint32_t x = 0; x < k_extent.width; ++x) {
            const glm::ivec4 c = img->rgba8(x, y);
            sum += 0.2126 * c.r + 0.7152 * c.g + 0.0722 * c.b;
        }
    }
    return static_cast<float>(sum / (k_extent.width * k_extent.height));
}

} // namespace

TEST(Photometry, MeteringWeightIsACentredGaussianOverAFloor) {
    EXPECT_FLOAT_EQ(ph::metering_weight(0.5F, 0.5F, 0.75F), 1.0F);
    EXPECT_FLOAT_EQ(ph::metering_weight(0.0F, 0.0F, 0.0F), 1.0F); // even metering
    const float corner = ph::metering_weight(0.0F, 1.0F, 0.75F);
    EXPECT_NEAR(corner, 0.25F + 0.75F * std::exp(-1.0F / 0.32F), 1e-6F);
    EXPECT_LT(ph::metering_weight(0.0F, 1.0F, 1.0F), 0.05F);
    EXPECT_GT(ph::metering_weight(0.5F, 0.3F, 0.75F), ph::metering_weight(0.5F, 0.1F, 0.75F));
}

// A back-lit subject: a dim disc in the middle of a sky 6 EV brighter.
// Centre weighting (default 0.75) meters the scene 0.77 EV darker than
// even metering in this layout (the subject is shown that much brighter);
// the test asks for at least half a stop. The GPU matches the CPU mirror.
TEST(GpuExposureMetering, CentreWeightingFavoursABackLitSubject) {
    QUAT_REQUIRE_GPU(g);
    const float subject = grey_card(2000.0F);
    const float sky = subject * 64.0F;
    const Scene scene = two_regions(subject, sky, [](float u, float v) {
        return (u - 0.5F) * (u - 0.5F) + (v - 0.5F) * (v - 0.5F) < 0.25F * 0.25F;
    });
    auto hdr = HdrImage::create(*g);
    ASSERT_OK(hdr);
    ASSERT_OK(hdr->fill(scene, 1.0F));
    float metered[2] = {};
    const float weights[2] = {0.0F, 0.75F};
    for (int i = 0; i < 2; ++i) {
        auto pass = r::ExposurePass::create(g->device, g->allocator, g->cache, g->shader_dir);
        ASSERT_OK(pass);
        r::ExposureMetering m;
        m.center_weight = weights[i];
        m.reset = true;
        ASSERT_OK(meter(*g, *hdr, *pass, 1.0F / 30.0F, m));
        metered[i] = std::log2(pass->adapted_luminance());
        EXPECT_NEAR(metered[i], cpu_metered_log2(scene, weights[i]), 0.02F)
            << "centre weight " << weights[i];
    }
    EXPECT_LT(metered[1], metered[0] - 0.5F) << "centre weighting barely moved the meter";
}

// Calibration bands of the displayed image (see the header comment).
TEST(GpuExposureMetering, DaylightAndNightScenesLandInTheirLumaBands) {
    QUAT_REQUIRE_GPU(g);
    // Daylight: the card under 100 klux in the lower 70 %, a clear sky of
    // 8000 cd/m² above.
    const Scene day = two_regions(grey_card(1.0e5F), 8000.0F / ph::k_nits_per_unit,
                                  [](float, float v) { return v > 0.3F; });
    // Night: the card under the 1 mlux floor, the night dome above.
    const Scene night = two_regions(grey_card(ph::k_night_sky_floor_lux),
                                    ph::k_night_sky_floor_lux / 3.14159265F / ph::k_nits_per_unit,
                                    [](float, float v) { return v > 0.3F; });
    auto day_luma = displayed_mean_luma(*g, day, 8);
    ASSERT_OK(day_luma);
    EXPECT_GT(*day_luma, 110.0F);
    EXPECT_LT(*day_luma, 150.0F);
    auto night_luma = displayed_mean_luma(*g, night, 8);
    ASSERT_OK(night_luma);
    RecordProperty("day_mean_luma", std::to_string(*day_luma));
    RecordProperty("night_mean_luma", std::to_string(*night_luma));
    EXPECT_GT(*night_luma, 4.0F);
    EXPECT_LT(*night_luma, 40.0F);
    EXPECT_LT(*night_luma, *day_luma * 0.5F) << "a night must look darker than a day";
}
