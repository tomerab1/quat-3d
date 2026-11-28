// HDRI image-based lighting (R21), read back from the GPU bake and checked
// against CPU references integrated over the source panorama: the
// prefiltered specular mips (GGX lobe, filtered importance sampling), the
// SH9 irradiance, a constant environment staying constant through every
// map, the sun's extraction (direction, energy, removal from the maps) and
// the environment rotation.

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <expected>
#include <functional>
#include <vector>

#include <glm/glm.hpp>
#include <gtest/gtest.h>
#include <vulkan/vulkan.h>

#include "engine/asset/hdr_decode.hpp"
#include "engine/renderer/hdri_environment.hpp"
#include "engine/renderer/hdri_math.hpp"
#include "engine/renderer/hdri_sun.hpp"
#include "engine/rhi/render_graph.hpp"
#include "support/expect_ok.hpp"
#include "support/gpu_context.hpp"
#include "support/gpu_submit.hpp"
#include "support/render_readback.hpp"

namespace {

namespace r = engine::renderer;
namespace rhi = engine::rhi;
namespace asset = engine::asset;
using engine::core::Error;

using Radiance = std::function<glm::vec3(const glm::vec3&)>;

asset::HdrImage panorama(std::uint32_t w, std::uint32_t h, const Radiance& f) {
    asset::HdrImage img;
    img.width = w;
    img.height = h;
    img.pixels = asset::PixelBuffer::allocate(asset::hdr_decoded_bytes(w, h));
    auto* p = reinterpret_cast<float*>(img.pixels.data());
    for (std::uint32_t y = 0; y < h; ++y) {
        for (std::uint32_t x = 0; x < w; ++x) {
            const glm::vec3 d = r::equirect_direction(
                {(static_cast<float>(x) + 0.5F) / static_cast<float>(w),
                 (static_cast<float>(y) + 0.5F) / static_cast<float>(h)});
            const glm::vec3 c = f(d);
            float* t = p + (static_cast<std::size_t>(y) * w + x) * 3;
            t[0] = c.r;
            t[1] = c.g;
            t[2] = c.b;
        }
    }
    return img;
}

// Sum over the panorama's texels of weight(l) * L(l) dw / sum of weight dw.
glm::vec3 integrate(const asset::HdrImage& img, const std::function<float(const glm::vec3&)>& w) {
    const auto rgb = img.rgb();
    glm::dvec3 num{0.0};
    double den = 0.0;
    for (std::uint32_t y = 0; y < img.height; ++y) {
        const double dw = r::equirect_texel_solid_angle(y, img.width, img.height);
        for (std::uint32_t x = 0; x < img.width; ++x) {
            const glm::vec3 l = r::equirect_direction(
                {(static_cast<float>(x) + 0.5F) / static_cast<float>(img.width),
                 (static_cast<float>(y) + 0.5F) / static_cast<float>(img.height)});
            const double weight = w(l) * dw;
            if (weight <= 0.0) continue;
            const float* t = &rgb[(static_cast<std::size_t>(y) * img.width + x) * 3];
            num += glm::dvec3(t[0], t[1], t[2]) * weight;
            den += weight;
        }
    }
    return den > 0.0 ? glm::vec3(num / den) : glm::vec3(0.0F);
}

// The split-sum prefilter integral with N = V = n: radiance weighted by
// (n.l) D(h) / 4, h = normalize(n + l), over the hemisphere around n.
glm::vec3 prefilter_reference(const asset::HdrImage& img, const glm::vec3& n, float rough) {
    const float a2 = rough * rough * rough * rough;
    return integrate(img, [&](const glm::vec3& l) {
        const float ndotl = glm::dot(n, l);
        if (ndotl <= 0.0F) return 0.0F;
        const glm::vec3 hv = glm::normalize(n + l);
        const float ndoth = glm::dot(n, hv);
        const float d = ndoth * ndoth * (a2 - 1.0F) + 1.0F;
        return ndotl * a2 / (r::k_hdri_pi * d * d) * 0.25F;
    });
}

// E(n) / pi: the cosine-weighted average radiance (the irradiance cube's unit).
glm::vec3 irradiance_reference(const asset::HdrImage& img, const glm::vec3& n) {
    // integrate() normalises by the weights' integral (pi for the cosine).
    return integrate(img, [&](const glm::vec3& l) { return std::max(glm::dot(n, l), 0.0F); });
}

const glm::vec3 k_lobe_dir = glm::normalize(glm::vec3(0.5F, 0.6F, -0.6F));

// A sky gradient plus a soft bright lobe, smooth everywhere: filtered
// importance sampling blurs each sample over a footprint (by design), which
// biases the result only where the radiance has kinks (a hard horizon), so
// the reference comparison uses a smooth environment.
glm::vec3 smooth_sky(const glm::vec3& d) {
    const float lobe = std::pow(std::max(glm::dot(d, k_lobe_dir), 0.0F), 16.0F);
    const float up = 0.5F + 0.5F * d.y;
    return (0.2F + 0.8F * up * up) * glm::vec3(1.0F, 0.9F, 0.8F) +
           3.0F * lobe * glm::vec3(1.0F, 0.8F, 0.6F);
}

std::expected<r::HdriEnvironment, Error> baked(quat::test::GpuContext& g,
                                               const asset::HdrImage& img,
                                               const r::HdriBakeSettings& settings = {}) {
    auto env = r::HdriEnvironment::create(g.device, g.allocator, g.cache, g.shader_dir);
    if (!env) return std::unexpected(env.error());
    if (auto s = env->set_image(img, g.transfer); !s) return std::unexpected(s.error());
    rhi::RenderGraph graph;
    auto declared = env->add_bake_to_graph(graph, settings);
    if (!declared) return std::unexpected(declared.error());
    if (!*declared) return std::unexpected(Error{"bake not declared"});
    if (auto c = graph.compile(); !c) return std::unexpected(c.error());
    auto ran = quat::test::submit_and_wait(g.device, [&](VkCommandBuffer cmd) { graph.execute(cmd); });
    if (!ran) return std::unexpected(ran.error());
    return env;
}

glm::vec3 texel(const std::vector<glm::vec4>& t, std::uint32_t size, std::uint32_t x,
                std::uint32_t y) {
    return glm::vec3(t[static_cast<std::size_t>(y) * size + x]);
}

void expect_rel_near(const glm::vec3& got, const glm::vec3& want, float rel, const char* what) {
    for (int c = 0; c < 3; ++c) {
        EXPECT_NEAR(got[c], want[c], want[c] * rel + 2e-3F)
            << what << " channel " << c << " got " << got[c] << " want " << want[c];
    }
}

} // namespace

TEST(HdriEnvironment, FaceSizeTracksThePanoramaWidth) {
    EXPECT_EQ(r::HdriEnvironment::face_size_for(2048), 512U);
    EXPECT_EQ(r::HdriEnvironment::face_size_for(2000), 256U);
    EXPECT_EQ(r::HdriEnvironment::face_size_for(16384), r::HdriEnvironment::k_max_face);
    EXPECT_EQ(r::HdriEnvironment::face_size_for(8), r::HdriEnvironment::k_min_face);
}

TEST(HdriEnvironment, EquirectMappingRoundTrips) {
    for (const glm::vec3 d : {glm::vec3(0, 0, -1), glm::vec3(1, 0, 0), glm::vec3(0.3F, 0.8F, 0.2F),
                              glm::vec3(-0.6F, -0.3F, 0.7F)}) {
        const glm::vec3 n = glm::normalize(d);
        const glm::vec3 back = r::equirect_direction(r::equirect_uv(n));
        EXPECT_NEAR(glm::dot(n, back), 1.0F, 1e-5F);
        const glm::vec3 rot = r::panorama_to_world(r::world_to_panorama(n, 1.1F), 1.1F);
        EXPECT_NEAR(glm::dot(n, rot), 1.0F, 1e-5F);
    }
    // -Z looks at the panorama's centre column; +90 degrees turns -Z to -X.
    EXPECT_NEAR(r::equirect_uv({0, 0, -1}).x, 0.5F, 1e-6F);
    const glm::vec3 turned = r::panorama_to_world({0, 0, -1}, glm::radians(90.0F));
    EXPECT_NEAR(turned.x, -1.0F, 1e-5F);
}

// The sun of a synthetic sky: found, its direction and clamped-off energy.
TEST(HdriEnvironment, SunAnalysisFindsDirectionAndEnergy) {
    const glm::vec3 sun_dir = glm::normalize(glm::vec3(-0.4F, 0.5F, 0.3F));
    const float radius = glm::radians(1.5F);
    const float cos_r = std::cos(radius);
    constexpr float k_sun = 20000.0F;
    const asset::HdrImage img = panorama(1024, 512, [&](const glm::vec3& d) {
        return glm::dot(d, sun_dir) > cos_r ? glm::vec3(k_sun) : glm::vec3(0.5F);
    });
    const r::HdriSun sun = r::analyze_hdri_sun(img);
    ASSERT_TRUE(sun.found);
    EXPECT_GT(glm::dot(sun.direction, sun_dir), std::cos(glm::radians(0.3F)));
    EXPECT_NEAR(sun.sky_luminance, 0.5F, 1e-3F);
    // Clamp-off of (k_sun - 2 * 0.5) over the disc's solid angle.
    const float disc = 2.0F * r::k_hdri_pi * (1.0F - cos_r);
    EXPECT_NEAR(sun.irradiance.g, (k_sun - 1.0F) * disc, (k_sun - 1.0F) * disc * 0.08F);

    // A sky without a sun (no texel 20x its surroundings) has none.
    const r::HdriSun none = r::analyze_hdri_sun(panorama(256, 128, smooth_sky));
    EXPECT_FALSE(none.found);
}

// Constant radiance stays constant through every map (the white furnace's
// input): irradiance, each prefiltered mip, and SH L00 = 4 pi Y00.
TEST(GpuHdriEnvironment, ConstantEnvironmentStaysConstant) {
    QUAT_REQUIRE_GPU(g);
    auto env = baked(*g, panorama(128, 64, [](const glm::vec3&) { return glm::vec3(1.0F); }));
    ASSERT_OK(env);
    ASSERT_EQ(env->face_size(), 32U);
    for (std::uint32_t face = 0; face < 6; ++face) {
        auto irr = quat::test::read_rgba16f(*g, env->irradiance_image(), 0, face,
                                            r::ibl_irradiance_size, r::ibl_irradiance_size);
        ASSERT_OK(irr);
        for (const glm::vec4& t : *irr) ASSERT_NEAR(t.g, 1.0F, 0.01F) << "irradiance face " << face;
        for (std::uint32_t mip = 0; mip < r::prefilter_mip_count; ++mip) {
            const std::uint32_t size = env->prefilter_face_size() >> mip;
            auto pre = quat::test::read_rgba16f(*g, env->prefiltered_image(), mip, face, size, size);
            ASSERT_OK(pre);
            for (const glm::vec4& t : *pre) ASSERT_NEAR(t.g, 1.0F, 0.01F) << "mip " << mip;
        }
    }
    const auto sh = env->sh9();
    EXPECT_NEAR(sh[0].g, 4.0F * r::k_hdri_pi * 0.282095F, 0.01F);
    for (std::size_t k = 1; k < 9; ++k) EXPECT_NEAR(sh[k].g, 0.0F, 0.01F) << "coefficient " << k;
}

// Every prefiltered mip against the CPU reference of the same GGX integral,
// at the centre texel of three faces.
TEST(GpuHdriEnvironment, PrefilteredMipsMatchTheCpuReference) {
    QUAT_REQUIRE_GPU(g);
    const asset::HdrImage img = panorama(256, 128, smooth_sky);
    auto env = baked(*g, img);
    ASSERT_OK(env);
    const std::uint32_t base = env->prefilter_face_size();
    for (const std::uint32_t face : {0U, 2U, 5U}) {
        for (std::uint32_t mip = 0; mip < r::prefilter_mip_count; ++mip) {
            const std::uint32_t size = base >> mip;
            auto pre = quat::test::read_rgba16f(*g, env->prefiltered_image(), mip, face, size, size);
            ASSERT_OK(pre);
            const std::uint32_t c = size / 2;
            const glm::vec2 uv((static_cast<float>(c) + 0.5F) / static_cast<float>(size));
            const glm::vec3 n = r::cube_direction(face, uv);
            const float rough =
                static_cast<float>(mip) / static_cast<float>(r::prefilter_mip_count - 1);
            // The mirror level is the environment itself (bilinear source).
            const glm::vec3 want = mip == 0 ? smooth_sky(n) : prefilter_reference(img, n, rough);
            SCOPED_TRACE(testing::Message() << "face " << face << " mip " << mip);
            expect_rel_near(texel(*pre, size, c, c), want, 0.05F, "prefiltered");
        }
    }
}

// SH9 irradiance against the exact cosine convolution of the panorama.
TEST(GpuHdriEnvironment, ShIrradianceMatchesTheCosineConvolution) {
    QUAT_REQUIRE_GPU(g);
    const asset::HdrImage img = panorama(256, 128, smooth_sky);
    auto env = baked(*g, img);
    ASSERT_OK(env);
    const std::uint32_t size = r::ibl_irradiance_size;
    for (std::uint32_t face = 0; face < 6; ++face) {
        auto irr = quat::test::read_rgba16f(*g, env->irradiance_image(), 0, face, size, size);
        ASSERT_OK(irr);
        for (const std::uint32_t x : {2U, 8U, 13U}) {
            const glm::vec2 uv((static_cast<float>(x) + 0.5F) / static_cast<float>(size),
                               (static_cast<float>(size - 1 - x) + 0.5F) / static_cast<float>(size));
            const glm::vec3 n = r::cube_direction(face, uv);
            SCOPED_TRACE(testing::Message() << "face " << face << " texel " << x);
            expect_rel_near(texel(*irr, size, x, size - 1 - x), irradiance_reference(img, n),
                            0.06F, "irradiance");
        }
    }
}

// A bright sun: removed from the maps (clamped to the sky level) when its
// energy moves to the analytic light, kept otherwise; rotation turns both.
TEST(GpuHdriEnvironment, SunIsRemovedAndRotated) {
    QUAT_REQUIRE_GPU(g);
    const glm::vec3 sun_dir = glm::normalize(glm::vec3(0.0F, 0.4F, -1.0F));
    const float cos_r = std::cos(glm::radians(2.0F));
    const asset::HdrImage img = panorama(512, 256, [&](const glm::vec3& d) {
        return glm::dot(d, sun_dir) > cos_r ? glm::vec3(30000.0F) : glm::vec3(0.5F);
    });
    // Brightest env texel and its direction.
    const auto brightest = [&](const r::HdriEnvironment& env) -> std::pair<float, glm::vec3> {
        const std::uint32_t f = env.face_size();
        float best = -1.0F;
        glm::vec3 dir{0.0F};
        for (std::uint32_t face = 0; face < 6; ++face) {
            auto t = quat::test::read_rgba16f(*g, env.environment_image(), 0, face, f, f);
            if (!t) return {-1.0F, dir};
            for (std::uint32_t i = 0; i < f * f; ++i) {
                if ((*t)[i].g > best) {
                    best = (*t)[i].g;
                    dir = r::cube_direction(face, {(static_cast<float>(i % f) + 0.5F) / f,
                                                   (static_cast<float>(i / f) + 0.5F) / f});
                }
            }
        }
        return {best, dir};
    };

    r::HdriBakeSettings keep;
    keep.remove_sun = false;
    keep.rotation_deg = 90.0F;
    auto kept = baked(*g, img, keep);
    ASSERT_OK(kept);
    const auto [kept_peak, kept_dir] = brightest(*kept);
    EXPECT_GT(kept_peak, 1000.0F);
    const glm::vec3 turned = r::panorama_to_world(sun_dir, glm::radians(90.0F));
    EXPECT_GT(glm::dot(kept_dir, turned), std::cos(glm::radians(3.0F)));
    const r::HdriSunLight light = kept->sun_light(keep);
    ASSERT_TRUE(light.found);
    EXPECT_GT(glm::dot(light.direction_to_sun, turned), std::cos(glm::radians(0.5F)));
    EXPECT_GT(light.intensity, 0.0F);

    auto removed = baked(*g, img, r::HdriBakeSettings{});
    ASSERT_OK(removed);
    const r::HdriSun& sun = removed->sun();
    ASSERT_TRUE(sun.found);
    EXPECT_LE(brightest(*removed).first, sun.clamp_luminance * 1.02F);
}

// Changing nothing declares nothing; a settings change re-bakes. A bake
// counts once its graph records it: a declared graph that never runs (a
// failed frame) leaves the maps unbaked, and the next frame declares again.
TEST(GpuHdriEnvironment, RebakesOnlyWhenSettingsChange) {
    QUAT_REQUIRE_GPU(g);
    auto env = r::HdriEnvironment::create(g->device, g->allocator, g->cache, g->shader_dir);
    ASSERT_OK(env);
    const auto run = [&](rhi::RenderGraph& graph) {
        ASSERT_OK(graph.compile());
        ASSERT_OK(quat::test::submit_and_wait(g->device,
                                              [&](VkCommandBuffer cmd) { graph.execute(cmd); }));
    };
    rhi::RenderGraph none;
    auto empty = env->add_bake_to_graph(none, {});
    ASSERT_OK(empty);
    EXPECT_FALSE(*empty); // no image yet
    ASSERT_OK(env->set_image(panorama(64, 32, smooth_sky), g->transfer));

    rhi::RenderGraph dropped; // declared, never recorded
    auto declared = env->add_bake_to_graph(dropped, {});
    ASSERT_OK(declared);
    EXPECT_TRUE(*declared);
    EXPECT_FALSE(env->baked());
    EXPECT_FALSE(env->views().valid()); // no unrecorded maps handed out
    EXPECT_EQ(env->environment_view(), VK_NULL_HANDLE);

    rhi::RenderGraph a;
    auto first = env->add_bake_to_graph(a, {});
    ASSERT_OK(first);
    EXPECT_TRUE(*first); // declared again: the first one never ran
    run(a);
    EXPECT_TRUE(env->baked());
    rhi::RenderGraph b;
    auto same = env->add_bake_to_graph(b, {});
    ASSERT_OK(same);
    EXPECT_FALSE(*same);
    r::HdriBakeSettings brighter;
    brighter.intensity = 2.0F;
    rhi::RenderGraph c;
    auto changed = env->add_bake_to_graph(c, brighter);
    ASSERT_OK(changed);
    EXPECT_TRUE(*changed);
    run(c);
    EXPECT_TRUE(env->views().valid());
    EXPECT_NE(env->environment_view(), VK_NULL_HANDLE);
}
