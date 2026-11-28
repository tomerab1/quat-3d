// The bright star field (R22.2.1) on a real device: stars are physical point
// sources whose splats carry exactly their illuminance —
//   - Sirius (V −1.46) renders 1.49 magnitudes (x3.94) brighter than Vega
//     (V +0.03) seen through the same air mass,
//   - a star's energy does not change as it drifts by a fraction of a pixel
//     (TAA-stable),
//   - at noon no star changes a single tonemapped pixel of the sky, while
//     at night the same star is plainly visible: daylight hides stars by
//     exposure alone,
//   - stars below the horizon and behind geometry are not drawn.

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <iterator>
#include <optional>
#include <span>
#include <expected>
#include <memory>
#include <vector>

#include <glm/glm.hpp>
#include <glm/gtc/matrix_transform.hpp>
#include <gtest/gtest.h>
#include <vulkan/vulkan.h>

#include "engine/astro/star_catalogue.hpp"
#include "engine/renderer/atmosphere_pass.hpp"
#include "engine/renderer/photometry.hpp"
#include "engine/renderer/star_field_pass.hpp"
#include "engine/rhi/render_graph.hpp"
#include "render/pbr_reference.hpp"
#include "support/expect_ok.hpp"
#include "support/gpu_commands.hpp"
#include "support/gpu_context.hpp"
#include "support/render_readback.hpp"

namespace {

namespace r = engine::renderer;
namespace rhi = engine::rhi;
namespace a = engine::astro;
using engine::core::Error;

constexpr VkExtent2D k_extent{96, 96};
constexpr VkFormat   k_hdr_format = VK_FORMAT_R16G16B16A16_SFLOAT;
constexpr VkFormat   k_depth_format = VK_FORMAT_R32_SFLOAT;
constexpr VkImageLayout k_ro = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
constexpr float      k_fov_y = 0.6F; // radians

// A direction at `elevation_deg` above the horizon, `azimuth_deg` around +Y.
[[nodiscard]] glm::vec3 sky_dir(float elevation_deg, float azimuth_deg) {
    const float e = glm::radians(elevation_deg);
    const float az = glm::radians(azimuth_deg);
    return {std::cos(e) * std::sin(az), std::sin(e), -std::cos(e) * std::cos(az)};
}

struct View {
    glm::mat4 view{1.0F};
    glm::mat4 proj{1.0F};
    glm::vec3 forward{0.0F, 0.0F, -1.0F};
};

// A camera at the origin looking at `target`, projection as the engine
// builds it (depth [0, 1], Vulkan's y flip).
[[nodiscard]] View look_at(const glm::vec3& target) {
    View v;
    v.forward = glm::normalize(target);
    v.view = glm::lookAt(glm::vec3(0.0F), v.forward, glm::vec3(0.0F, 1.0F, 0.0F));
    v.proj = glm::perspective(k_fov_y, 1.0F, 0.1F, 1000.0F);
    v.proj[1][1] *= -1.0F;
    return v;
}

// A colour image cleared to `value` and left in SHADER_READ_ONLY_OPTIMAL.
struct ClearedImage {
    rhi::GpuImage  image;
    rhi::ImageView view;
    VkFormat       format = VK_FORMAT_UNDEFINED;
    float          value = 0.0F;

    [[nodiscard]] static std::expected<ClearedImage, Error>
    create(quat::test::GpuContext& g, VkFormat format, VkImageUsageFlags usage) {
        ClearedImage out;
        out.format = format;
        auto img = g.allocator.create_image(
            format, k_extent,
            usage | VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT |
                VK_IMAGE_USAGE_TRANSFER_SRC_BIT);
        if (!img) return std::unexpected(img.error());
        auto view = rhi::create_image_view(g.device.handle(), img->handle(), format);
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
                                  VK_ACCESS_2_SHADER_SAMPLED_READ_BIT |
                                      VK_ACCESS_2_SHADER_STORAGE_READ_BIT);
    }
};

struct Rig {
    std::optional<r::AtmospherePass> atmosphere;
    std::optional<r::StarFieldPass>  pass;
    ClearedImage                     hdr;
    ClearedImage                     depth;
};

[[nodiscard]] std::expected<Rig, Error> make_rig(quat::test::GpuContext& g,
                                                 std::span<const a::CatalogueStar> stars) {
    Rig rig;
    auto atmos = r::AtmospherePass::create(g.device, g.allocator, g.cache, g.shader_dir);
    if (!atmos) return std::unexpected(atmos.error());
    rig.atmosphere = std::move(*atmos);
    auto catalogue = r::StarCatalogueGpu::create(g.device, g.allocator, stars);
    if (!catalogue) return std::unexpected(catalogue.error());
    auto pass = r::StarFieldPass::create(g.device, g.allocator, g.cache, g.shader_dir,
                                         std::move(*catalogue));
    if (!pass) return std::unexpected(pass.error());
    rig.pass = std::move(*pass);
    auto hdr = ClearedImage::create(g, k_hdr_format, VK_IMAGE_USAGE_STORAGE_BIT);
    if (!hdr) return std::unexpected(hdr.error());
    rig.hdr = std::move(*hdr);
    auto depth = ClearedImage::create(g, k_depth_format, 0);
    if (!depth) return std::unexpected(depth.error());
    rig.depth = std::move(*depth);
    rig.depth.value = 1.0F; // the far plane: sky everywhere
    return rig;
}

// Renders the stars over an HDR cleared to `background` and returns the
// HDR (pre-exposed with `pre`).
[[nodiscard]] std::expected<quat::test::HostImage, Error>
render(quat::test::GpuContext& g, Rig& rig, const View& v, float background, float pre,
       const glm::mat4& jitter = glm::mat4(1.0F)) {
    rig.hdr.value = background;
    rhi::TransientImagePool pool(g.device, g.allocator);
    rhi::RenderGraph graph(pool);
    const rhi::ResourceHandle hdr = graph.import_image(
        "test_hdr", rig.hdr.image.handle(), rig.hdr.view.handle(), k_hdr_format, k_extent, k_ro, k_ro);
    const rhi::ResourceHandle depth =
        graph.import_image("test_depth", rig.depth.image.handle(), rig.depth.view.handle(),
                           k_depth_format, k_extent, k_ro, k_ro);
    r::StarFieldFrame frame;
    frame.view_proj = jitter * v.proj * v.view;
    frame.projection = v.proj;
    frame.camera_forward = v.forward;
    frame.world_from_celestial = glm::mat3(1.0F); // catalogue vectors are world directions
    frame.extent = k_extent;
    frame.pre_exposure = pre;
    frame.scintillation = 0.0F; // steady
    frame.clouds.enabled = false;
    if (auto a = rig.pass->add_to_graph(graph, hdr, depth, rig.atmosphere->transmittance_view(),
                                        frame);
        !a) {
        return std::unexpected(a.error());
    }
    return quat::test::render_to_host(g, graph, hdr, k_extent, 8, [&](VkCommandBuffer cmd) {
        rig.hdr.clear(cmd);
        rig.depth.clear(cmd);
    });
}

// Pixel solid angle at pixel (x, y) of the test projection.
[[nodiscard]] float pixel_solid_angle(const View& v, std::uint32_t x, std::uint32_t y) {
    const float w = static_cast<float>(k_extent.width);
    const float h = static_cast<float>(k_extent.height);
    const float omega0 = 4.0F / (w * std::abs(v.proj[0][0]) * h * std::abs(v.proj[1][1]));
    const float nx = ((static_cast<float>(x) + 0.5F) / w * 2.0F - 1.0F) / std::abs(v.proj[0][0]);
    const float ny = ((static_cast<float>(y) + 0.5F) / h * 2.0F - 1.0F) / std::abs(v.proj[1][1]);
    const float cos_axis = 1.0F / std::sqrt(1.0F + nx * nx + ny * ny);
    return omega0 * cos_axis * cos_axis * cos_axis;
}

// Illuminance (lux) of the splat in the window [x0, x1) x [y0, y1), above
// the cleared background: sum of L * Omega, undoing the pre-exposure.
[[nodiscard]] double window_lux(const quat::test::HostImage& img, const View& v, float pre,
                                std::uint32_t x0, std::uint32_t x1) {
    double sum = 0.0;
    for (std::uint32_t y = 0; y < k_extent.height; ++y) {
        for (std::uint32_t x = x0; x < x1; ++x) {
            const glm::vec4 c = img.rgba16f(x, y);
            const double lum = 0.2126 * c.r + 0.7152 * c.g + 0.0722 * c.b;
            sum += lum * pixel_solid_angle(v, x, y);
        }
    }
    return sum / pre * r::photometry::k_nits_per_unit;
}

[[nodiscard]] int srgb8(float hdr_exposed) {
    const float mapped = quat::test::pbr_ref::aces(glm::vec3(hdr_exposed)).x;
    return static_cast<int>(std::round(quat::test::pbr_ref::linear_to_srgb(mapped) * 255.0F));
}

} // namespace

// Sirius and Vega side by side at the same elevation (same air mass): the
// splat energies differ by the catalogue's 1.49 magnitudes, and each is the
// star's illuminance times a plausible transmittance.
TEST(GpuStarField, SiriusIsOnePointFourNineMagnitudesBrighterThanVega) {
    QUAT_REQUIRE_GPU(g);
    const glm::vec3 sirius_dir = sky_dir(60.0F, -6.0F);
    const glm::vec3 vega_dir = sky_dir(60.0F, 6.0F);
    const std::vector<a::CatalogueStar> stars{{sirius_dir, -1.46F, 0.0F},
                                              {vega_dir, 0.03F, 0.0F}};
    auto rig = make_rig(*g, stars);
    ASSERT_OK(rig);
    const View v = look_at(sky_dir(60.0F, 0.0F));
    constexpr float k_pre = 2.0e4F; // keeps the splats well inside RGBA16F
    auto img = render(*g, *rig, v, 0.0F, k_pre);
    ASSERT_OK(img);
    // Sirius is left of centre, Vega right (azimuth grows clockwise from -Z).
    const std::uint32_t mid = k_extent.width / 2;
    const double sirius = window_lux(*img, v, k_pre, 0, mid);
    const double vega = window_lux(*img, v, k_pre, mid, k_extent.width);
    ASSERT_GT(vega, 0.0);
    const double expected = std::pow(10.0, 0.4 * (0.03 + 1.46)); // 3.94
    EXPECT_NEAR(sirius / vega, expected, expected * 0.03);
    // Above the atmosphere Sirius gives 9.7 µlux; through 1.15 air masses
    // the clear-sky transmittance is 0.7-0.95.
    const double top = a::star_illuminance_lux(-1.46);
    EXPECT_GT(sirius, 0.7 * top);
    EXPECT_LT(sirius, 0.95 * top);
}

// A star drifting by fractions of a pixel keeps its energy (the PSF weights
// are normalised), so the TAA resolve sees a steady point.
TEST(GpuStarField, SplatEnergyIsIndependentOfSubPixelPosition) {
    QUAT_REQUIRE_GPU(g);
    const std::vector<a::CatalogueStar> stars{{sky_dir(45.0F, 0.0F), 0.0F, 0.6F}};
    auto rig = make_rig(*g, stars);
    ASSERT_OK(rig);
    const View v = look_at(sky_dir(45.0F, 0.0F));
    constexpr float k_pre = 2.0e4F;
    double first = -1.0;
    for (int i = 0; i < 6; ++i) {
        // Shift the clip-space image by i/6 of a pixel diagonally (as the
        // TAA jitter does).
        const float px = static_cast<float>(i) / 6.0F;
        const glm::mat4 jitter = glm::translate(
            glm::mat4(1.0F), glm::vec3(2.0F * px / static_cast<float>(k_extent.width),
                                       2.0F * px / static_cast<float>(k_extent.height), 0.0F));
        auto img = render(*g, *rig, v, 0.0F, k_pre, jitter);
        ASSERT_OK(img);
        const double lux = window_lux(*img, v, k_pre, 0, k_extent.width);
        ASSERT_GT(lux, 0.0);
        if (first < 0.0) first = lux;
        EXPECT_NEAR(lux, first, first * 0.01) << "shift " << px << " px";
    }
}

// Daylight hides the stars by exposure alone: over a clear noon sky (at
// least 3000 cd/m² away from the sun), exposed as auto exposure exposes it,
// Sirius at the zenith changes no tonemapped 8-bit pixel. At night, over a
// moonless sky, the same star stands far above the background.
TEST(GpuStarField, StarsAreInvisibleAtNoonAndBrightAtNight) {
    QUAT_REQUIRE_GPU(g);
    const std::vector<a::CatalogueStar> stars{{sky_dir(89.0F, 0.0F), -1.46F, 0.0F}};
    auto rig = make_rig(*g, stars);
    ASSERT_OK(rig);
    const View v = look_at(sky_dir(89.0F, 0.0F));
    namespace ph = r::photometry;
    const struct {
        const char* name;
        float       sky_nits;
    } cases[] = {{"noon", 3000.0F}, {"night", 2e-4F}};
    for (std::size_t i = 0; i < std::size(cases); ++i) {
        const auto& c = cases[i];
        const float sky = c.sky_nits / ph::k_nits_per_unit;
        const float exposure = ph::auto_exposure(sky, 0.18F, true, -14.0F, 20.0F);
        auto img = render(*g, *rig, v, sky * exposure, exposure);
        ASSERT_OK(img);
        const int background = srgb8(sky * exposure);
        int brightest = background;
        for (std::uint32_t y = 0; y < k_extent.height; ++y) {
            for (std::uint32_t x = 0; x < k_extent.width; ++x) {
                brightest = std::max(brightest, srgb8(img->rgba16f(x, y).g));
            }
        }
        if (i == 0) {
            EXPECT_EQ(brightest, background) << "a star shows in the noon sky";
            // Physics, not the pass's cull of invisible stars: even with
            // all of Sirius's energy above the atmosphere in one pixel, the
            // noon sky pixel keeps its 8-bit code.
            const float omega = pixel_solid_angle(v, k_extent.width / 2, k_extent.height / 2);
            const float peak = static_cast<float>(a::star_illuminance_lux(-1.46)) / omega /
                               ph::k_nits_per_unit;
            EXPECT_EQ(srgb8((sky + peak) * exposure), background)
                << "Sirius' peak " << peak << " over a sky of " << sky;
        } else {
            EXPECT_GT(brightest, background + 100) << "the star is lost in the night sky";
        }
    }
}

// Below the horizon the planet hides the stars; behind geometry (depth < 1)
// too.
TEST(GpuStarField, StarsBelowTheHorizonOrBehindGeometryAreHidden) {
    QUAT_REQUIRE_GPU(g);
    const std::vector<a::CatalogueStar> stars{{sky_dir(-3.0F, -3.0F), -1.46F, 0.0F},
                                              {sky_dir(4.0F, 3.0F), -1.46F, 0.0F}};
    auto rig = make_rig(*g, stars);
    ASSERT_OK(rig);
    const View v = look_at(sky_dir(0.0F, 0.0F));
    constexpr float k_pre = 2.0e4F;
    auto img = render(*g, *rig, v, 0.0F, k_pre);
    ASSERT_OK(img);
    const std::uint32_t mid = k_extent.width / 2;
    EXPECT_EQ(window_lux(*img, v, k_pre, 0, mid), 0.0) << "a star below the horizon drew";
    EXPECT_GT(window_lux(*img, v, k_pre, mid, k_extent.width), 0.0);

    rig->depth.value = 0.5F; // geometry everywhere
    auto covered = render(*g, *rig, v, 0.0F, k_pre);
    ASSERT_OK(covered);
    EXPECT_EQ(window_lux(*covered, v, k_pre, 0, k_extent.width), 0.0);
}
