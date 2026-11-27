// R20 tonemap pass "look": every operator and the scene-referred grade match
// their CPU references (color_grading.hpp) pixel for pixel, an identity look
// LUT is a no-op while a real one is applied, the physical camera's exposure
// replaces auto exposure, and the lens / film effects behave (vignette darkens
// the corners only, grain adds zero-mean noise, chromatic aberration leaves a
// flat image alone).

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <expected>
#include <vector>

#include <glm/glm.hpp>
#include <gtest/gtest.h>
#include <vulkan/vulkan.h>

#include "engine/asset/cube_lut.hpp"
#include "engine/renderer/color_grading.hpp"
#include "engine/renderer/color_lut.hpp"
#include "engine/renderer/physical_camera.hpp"
#include "engine/renderer/tonemap_pass.hpp"
#include "engine/rhi/render_graph.hpp"
#include "render/post_image.hpp"
#include "support/expect_ok.hpp"
#include "support/gpu_context.hpp"
#include "support/render_readback.hpp"

namespace {

namespace r = engine::renderer;
namespace rhi = engine::rhi;
namespace gr = engine::renderer::grading;
using engine::core::Error;
using quat::test::HostImage;

constexpr VkFormat   k_ldr_format = VK_FORMAT_R8G8B8A8_UNORM;
constexpr VkExtent2D k_strip{16, 4};
constexpr VkExtent2D k_flat{64, 64};

// One colour per column: greys over 10 stops, saturated and bright colours.
std::vector<glm::vec3> strip_colours() {
    std::vector<glm::vec3> c;
    for (int i = 0; i < 8; ++i) c.emplace_back(0.18F * std::exp2(static_cast<float>(i) - 4.0F));
    c.emplace_back(0.8F, 0.1F, 0.05F);
    c.emplace_back(0.05F, 0.6F, 0.1F);
    c.emplace_back(0.1F, 0.2F, 0.9F);
    c.emplace_back(4.0F, 1.0F, 0.2F);
    c.emplace_back(20.0F, 18.0F, 12.0F);
    c.emplace_back(0.0F);
    c.emplace_back(0.03F, 0.02F, 0.01F);
    c.emplace_back(1.5F, 0.4F, 2.5F);
    return c;
}

std::expected<quat::test::PostImage, Error> upload_strip(quat::test::GpuContext& g) {
    const std::vector<glm::vec3> colours = strip_colours();
    std::vector<glm::vec4> texels;
    for (std::uint32_t y = 0; y < k_strip.height; ++y) {
        for (const glm::vec3& c : colours) texels.emplace_back(c, 1.0F);
    }
    return quat::test::upload_rgba16f(g, k_strip, texels);
}

std::expected<HostImage, Error> run(quat::test::GpuContext& g, const quat::test::PostImage& hdr,
                                    const r::TonemapLook& look, float compensation_ev = 0.0F) {
    auto pass = r::TonemapPass::create(g.device, g.allocator, g.cache, g.shader_dir, k_ldr_format);
    if (!pass) return std::unexpected(pass.error());
    rhi::TransientImagePool pool(g.device, g.allocator);
    rhi::RenderGraph graph(pool);
    const rhi::ResourceHandle in = hdr.import(graph, "test_hdr");
    const rhi::ResourceHandle out = graph.create_transient_image(
        "ldr", k_ldr_format, hdr.extent,
        VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT);
    r::ExposureSettings manual;
    manual.auto_exposure = false;
    manual.compensation_ev = compensation_ev;
    if (auto a = pass->add_to_graph(graph, in, out, hdr.extent, 0, manual, look); !a) {
        return std::unexpected(a.error());
    }
    return quat::test::render_to_host(g, graph, out, hdr.extent, 4);
}

// The CPU reference of one pixel without LUT / film effects, as 0..255.
glm::ivec3 reference(const r::TonemapLook& look, glm::vec3 hdr, float exposure = 1.0F) {
    const glm::vec3 graded = gr::grade(gr::make_grading_gpu(look.grading), hdr * exposure);
    const glm::vec3 mapped = gr::tonemap(look.op, graded);
    glm::ivec3 out;
    for (int k = 0; k < 3; ++k) {
        out[k] = static_cast<int>(std::round(std::clamp(gr::linear_to_srgb(mapped[k]), 0.0F, 1.0F) * 255.0F));
    }
    return out;
}

void expect_strip_matches(const HostImage& img, const r::TonemapLook& look, float exposure,
                          int tolerance, const char* what) {
    const std::vector<glm::vec3> colours = strip_colours();
    for (std::uint32_t x = 0; x < colours.size(); ++x) {
        const glm::ivec3 expected = reference(look, colours[x], exposure);
        const glm::ivec4 px = img.rgba8(x, 2);
        for (int k = 0; k < 3; ++k) {
            EXPECT_NEAR(px[k], expected[k], tolerance) << what << " column " << x << " channel " << k;
        }
    }
}

TEST(GpuTonemapLook, OperatorsMatchTheirReferences) {
    QUAT_REQUIRE_GPU(g);
    auto strip = upload_strip(*g);
    ASSERT_OK(strip);
    for (const r::TonemapOperator op :
         {r::TonemapOperator::aces, r::TonemapOperator::agx, r::TonemapOperator::pbr_neutral}) {
        r::TonemapLook look;
        look.op = op;
        auto img = run(*g, *strip, look);
        ASSERT_OK(img);
        expect_strip_matches(*img, look, 1.0F, 1, std::string(r::tonemap_operator_names[static_cast<int>(op)]).c_str());
    }
}

TEST(GpuTonemapLook, GradingMatchesTheReference) {
    QUAT_REQUIRE_GPU(g);
    auto strip = upload_strip(*g);
    ASSERT_OK(strip);
    r::TonemapLook look;
    look.op = r::TonemapOperator::agx;
    look.grading.temperature_k = 4300.0F;
    look.grading.tint = 0.2F;
    look.grading.contrast = 1.25F;
    look.grading.saturation = 1.3F;
    look.grading.lift_b = 0.03F;
    look.grading.gamma_g = 1.1F;
    look.grading.gain_r = 1.08F;
    auto img = run(*g, *strip, look);
    ASSERT_OK(img);
    expect_strip_matches(*img, look, 1.0F, 1, "graded");
}

TEST(GpuTonemapLook, PhysicalExposureReplacesTheMultiplier) {
    QUAT_REQUIRE_GPU(g);
    auto strip = upload_strip(*g);
    ASSERT_OK(strip);
    r::PhysicalCamera cam;
    cam.aperture_f = 1.4F;
    cam.shutter_s = 0.5F;
    cam.iso = 100.0F; // EV100 = log2(1.96 / 0.5) = 1.97
    r::TonemapLook look;
    look.physical_exposure = r::exposure_from_ev100(r::ev100(cam));
    auto img = run(*g, *strip, look, /*compensation_ev=*/1.0F);
    ASSERT_OK(img);
    expect_strip_matches(*img, look, look.physical_exposure * 2.0F, 1, "physical");
}

std::expected<r::ColorLutTexture, Error> upload_lut(quat::test::GpuContext& g,
                                                    const engine::asset::CubeLut& lut) {
    return r::ColorLutTexture::create(g.allocator, g.transfer, lut);
}

TEST(GpuTonemapLook, IdentityLutIsANoOpAndARealLutApplies) {
    QUAT_REQUIRE_GPU(g);
    auto strip = upload_strip(*g);
    ASSERT_OK(strip);
    r::TonemapLook plain;
    plain.op = r::TonemapOperator::agx;
    auto base = run(*g, *strip, plain);
    ASSERT_OK(base);
    for (const std::uint32_t size : {2U, 17U, 33U}) {
        auto lut = upload_lut(*g, engine::asset::identity_cube_lut(size));
        ASSERT_OK(lut);
        r::TonemapLook look = plain;
        look.lut = &*lut;
        auto img = run(*g, *strip, look);
        ASSERT_OK(img);
        for (std::uint32_t x = 0; x < k_strip.width; ++x) {
            for (int k = 0; k < 3; ++k) {
                EXPECT_NEAR(img->rgba8(x, 1)[k], base->rgba8(x, 1)[k], 1)
                    << "identity " << size << " column " << x;
            }
        }
    }
    // An inverting LUT (out = 1 - in, linear in the encoded value, so exact
    // under trilinear filtering) flips the image; half strength meets mid-grey.
    engine::asset::CubeLut invert = engine::asset::identity_cube_lut(9);
    for (float& v : invert.rgb) v = 1.0F - v;
    auto inv = upload_lut(*g, invert);
    ASSERT_OK(inv);
    r::TonemapLook look = plain;
    look.lut = &*inv;
    auto flipped = run(*g, *strip, look);
    ASSERT_OK(flipped);
    look.grading.lut_contribution = 0.5F;
    auto half = run(*g, *strip, look);
    ASSERT_OK(half);
    for (std::uint32_t x = 0; x < k_strip.width; ++x) {
        for (int k = 0; k < 3; ++k) {
            EXPECT_NEAR(flipped->rgba8(x, 1)[k], 255 - base->rgba8(x, 1)[k], 1) << "column " << x;
            EXPECT_NEAR(half->rgba8(x, 1)[k], 128, 1) << "column " << x;
        }
    }
}

std::expected<quat::test::PostImage, Error> upload_flat(quat::test::GpuContext& g, float value) {
    const std::vector<glm::vec4> texels(static_cast<std::size_t>(k_flat.width) * k_flat.height,
                                        glm::vec4(value, value, value, 1.0F));
    return quat::test::upload_rgba16f(g, k_flat, texels);
}

TEST(GpuTonemapLook, VignetteDarkensTheCornersOnly) {
    QUAT_REQUIRE_GPU(g);
    auto flat = upload_flat(*g, 0.5F);
    ASSERT_OK(flat);
    auto base = run(*g, *flat, {});
    ASSERT_OK(base);
    r::TonemapLook look;
    look.fov_y = glm::radians(90.0F);
    look.film.vignette_natural = 1.0F;
    auto natural = run(*g, *flat, look);
    ASSERT_OK(natural);
    // The optical axis keeps its light; cos^4 of the corner ray darkens it.
    EXPECT_NEAR(natural->rgba8(32, 32).g, base->rgba8(32, 32).g, 1);
    EXPECT_LT(natural->rgba8(0, 0).g, base->rgba8(0, 0).g - 40);
    EXPECT_LT(natural->rgba8(0, 0).g, natural->rgba8(0, 32).g); // corner < edge
    look.film.vignette_natural = 0.0F;
    look.film.vignette_intensity = 0.8F;
    auto artistic = run(*g, *flat, look);
    ASSERT_OK(artistic);
    EXPECT_EQ(artistic->rgba8(32, 32).g, base->rgba8(32, 32).g);
    EXPECT_LT(artistic->rgba8(1, 1).g, base->rgba8(1, 1).g - 40);
}

TEST(GpuTonemapLook, GrainIsZeroMeanNoiseAndAberrationKeepsAFlatImage) {
    QUAT_REQUIRE_GPU(g);
    auto flat = upload_flat(*g, 0.3F);
    ASSERT_OK(flat);
    auto base = run(*g, *flat, {});
    ASSERT_OK(base);
    r::TonemapLook look;
    look.film.grain_intensity = 1.0F;
    look.time_s = 3.0F;
    auto grain = run(*g, *flat, look);
    ASSERT_OK(grain);
    double mean = 0.0;
    double var = 0.0;
    const int base_g = base->rgba8(5, 5).g;
    const double n = static_cast<double>(k_flat.width) * k_flat.height;
    for (std::uint32_t y = 0; y < k_flat.height; ++y) {
        for (std::uint32_t x = 0; x < k_flat.width; ++x) {
            const double d = grain->rgba8(x, y).g - base_g;
            mean += d / n;
            var += d * d / n;
        }
    }
    EXPECT_LT(std::abs(mean), 1.5);
    EXPECT_GT(std::sqrt(var), 2.0); // visible grain
    EXPECT_LT(std::sqrt(var), 20.0); // but subtle
    // A different time on the 24 Hz grain clock changes the pattern.
    look.time_s = 3.5F;
    auto later = run(*g, *flat, look);
    ASSERT_OK(later);
    int differing = 0;
    for (std::uint32_t x = 0; x < k_flat.width; ++x) differing += later->rgba8(x, 9).g != grain->rgba8(x, 9).g;
    EXPECT_GT(differing, 10);

    r::TonemapLook ca;
    ca.film.chromatic_aberration = 1.0F;
    auto aberrated = run(*g, *flat, ca);
    ASSERT_OK(aberrated);
    for (std::uint32_t y = 0; y < k_flat.height; y += 9) {
        for (std::uint32_t x = 0; x < k_flat.width; x += 9) {
            for (int k = 0; k < 3; ++k) EXPECT_NEAR(aberrated->rgba8(x, y)[k], base->rgba8(x, y)[k], 1);
        }
    }
}

} // namespace
