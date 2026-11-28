// Hashed alpha testing (R26.3, lib/alpha_test.slang): a minified cut-out
// "leaf" texture keeps the plain cutoff test's coverage on average, its
// dither is stable for a seed (no swimming) and decorrelated across seeds
// (so TAA integrates it), and the shadow casters use the same coverage.

#include <cmath>
#include <cstdint>
#include <vector>

#include <glm/glm.hpp>
#include <gtest/gtest.h>

#include "engine/asset/alpha_coverage.hpp"
#include "engine/asset/asset_manager.hpp"
#include "engine/asset/material_asset.hpp"
#include "engine/asset/texture_asset.hpp"
#include "render/shadow_scene_fixture.hpp"
#include "support/expect_ok.hpp"
#include "support/gpu_context.hpp"

namespace {

namespace a = engine::asset;
namespace r = engine::renderer;
namespace occ = quat::test::occlusion;
namespace ss = quat::test::shadow_scene;

constexpr std::uint32_t k_tex = 128;

// White texture, opaque in scattered 2x2-texel "leaves" (about 45 %).
a::TextureData leaves() {
    a::TextureData t;
    t.width = k_tex;
    t.height = k_tex;
    t.color_space = a::TextureColorSpace::srgb;
    t.pixels = a::PixelBuffer::allocate(static_cast<std::size_t>(k_tex) * k_tex * 4);
    for (std::uint32_t y = 0; y < k_tex; ++y) {
        for (std::uint32_t x = 0; x < k_tex; ++x) {
            const std::uint32_t h = ((x / 2) * 73856093U) ^ ((y / 2) * 19349663U);
            const bool opaque = (h % 100U) < 45U;
            std::byte* p = t.pixels.data() + (static_cast<std::size_t>(y) * k_tex + x) * 4;
            p[0] = p[1] = p[2] = std::byte{255};
            p[3] = opaque ? std::byte{255} : std::byte{0};
        }
    }
    EXPECT_TRUE(a::apply_alpha_tested_mips(t, 0.5F));
    return t;
}

struct Scene {
    a::AssetManager  assets;
    a::MeshAsset     floor;
    a::MeshAsset     quad;
    a::MaterialAsset floor_mat;
    a::MaterialAsset quad_mat;
};

// Red floor, green masked quad 1 m above it over x, z in [-1, 1].
void make_scene(quat::test::GpuContext& g, Scene& s) {
    auto floor = occ::upload_box(g, {-8.0F, -0.5F, -8.0F}, {8.0F, 0.0F, 8.0F});
    ASSERT_OK(floor);
    s.floor = std::move(*floor);
    auto quad = ss::upload_quad(g, 1.0F, 1.0F);
    ASSERT_OK(quad);
    s.quad = std::move(*quad);
    a::PbrMaterialParams red;
    red.base_color_factor = {0.8F, 0.1F, 0.1F, 1.0F};
    red.metallic_factor = 0.0F;
    red.roughness_factor = 1.0F;
    auto floor_mat = ss::upload_params(g, red);
    ASSERT_OK(floor_mat);
    s.floor_mat = std::move(*floor_mat);
    const auto texture = s.assets.load<a::TextureAsset>("test/leaves", [&](const auto&) {
        return a::upload_texture_data(leaves(), g.allocator, g.transfer);
    });
    ASSERT_TRUE(texture.is_loaded());
    a::PbrMaterialParams green = red;
    green.base_color_factor = {0.1F, 0.8F, 0.1F, 1.0F};
    green.alpha_cutoff = 0.5F;
    green.flags = a::material_has_base_color | a::material_alpha_mask;
    a::MaterialTextures textures;
    textures.base_color = texture;
    auto quad_mat = ss::upload_params(g, green, textures);
    ASSERT_OK(quad_mat);
    s.quad_mat = std::move(*quad_mat);
}

std::vector<r::DrawItem> draws(const Scene& s) {
    return {{&s.floor, &s.floor_mat, glm::mat4(1.0F)}, {&s.quad, &s.quad_mat, glm::mat4(1.0F)}};
}

// Green (quad) pixels of the image inside the quad's inner footprint.
struct Coverage {
    std::vector<bool> green;
    float             fraction = 0.0F;
};

Coverage quad_coverage(const quat::test::HostImage& img, const occ::Camera& cam) {
    const glm::uvec2 lo = occ::project(cam.view_proj, {-0.8F, 1.0F, -0.8F}, ss::k_extent);
    const glm::uvec2 hi = occ::project(cam.view_proj, {0.8F, 1.0F, 0.8F}, ss::k_extent);
    Coverage c;
    std::uint32_t n = 0;
    std::uint32_t covered = 0;
    for (std::uint32_t y = std::min(lo.y, hi.y); y <= std::max(lo.y, hi.y); ++y) {
        for (std::uint32_t x = std::min(lo.x, hi.x); x <= std::max(lo.x, hi.x); ++x) {
            const glm::ivec4 px = img.rgba8(x, y);
            const bool g = px.g > px.r;
            c.green.push_back(g);
            covered += g ? 1U : 0U;
            ++n;
        }
    }
    c.fraction = n > 0 ? static_cast<float>(covered) / static_cast<float>(n) : 0.0F;
    return c;
}

// High above the quad: the 128-texel leaves cover ~25 pixels (minified).
occ::Camera far_camera() { return occ::look_at({0.0F, 22.0F, 0.3F}, {0.0F, 0.0F, 0.0F}); }

} // namespace

TEST(GpuHashedAlpha, KeepsCoverageAndIsStablePerSeed) {
    QUAT_REQUIRE_GPU(g);
    Scene s;
    make_scene(*g, s);
    auto p = ss::make_passes(*g);
    ASSERT_OK(p);
    const occ::Camera cam = far_camera();
    const auto items = draws(s);

    p->mesh.set_alpha_hash(false, 0);
    auto plain = ss::render_albedo(*g, *p, cam, items);
    ASSERT_OK(plain);
    const Coverage cutoff = quad_coverage(*plain, cam);
    ASSERT_GT(cutoff.green.size(), 100U) << "quad too small on screen";

    float mean = 0.0F;
    std::vector<Coverage> frames;
    for (std::uint64_t seed = 0; seed < 8; ++seed) {
        p->mesh.set_alpha_hash(true, seed);
        auto img = ss::render_albedo(*g, *p, cam, items);
        ASSERT_OK(img);
        frames.push_back(quad_coverage(*img, cam));
        mean += frames.back().fraction / 8.0F;
    }
    // The cut-out's area survives hashing (TAA's average of the frames).
    EXPECT_NEAR(mean, cutoff.fraction, 0.12F) << "cutoff " << cutoff.fraction;
    EXPECT_GT(mean, 0.1F);
    EXPECT_LT(mean, 0.9F);

    // Same seed: the same pixels (the hash is stable, no swimming).
    p->mesh.set_alpha_hash(true, 0);
    auto again = ss::render_albedo(*g, *p, cam, items);
    ASSERT_OK(again);
    EXPECT_EQ(quad_coverage(*again, cam).green, frames[0].green);
    // Another seed: a different dither for the temporal resolve.
    std::size_t differ = 0;
    for (std::size_t i = 0; i < frames[0].green.size(); ++i) {
        differ += frames[0].green[i] != frames[1].green[i] ? 1U : 0U;
    }
    EXPECT_GT(differ, frames[0].green.size() / 20) << "seeds do not decorrelate the dither";
}

// The shadow casters' hashed coverage lets through about as much sun as the
// cutoff test's: a leaf shadows what it covers.
TEST(GpuHashedAlpha, ShadowCastersMatchTheGBufferCoverage) {
    QUAT_REQUIRE_GPU(g);
    Scene s;
    make_scene(*g, s);
    auto p = ss::make_passes(*g);
    ASSERT_OK(p);
    const occ::Camera cam = ss::shadow_camera();
    const auto items = draws(s);
    const auto transmitted = [&](bool hashed) {
        p->mesh.set_alpha_hash(hashed, 0);
        p->shadow.set_alpha_hash(hashed);
        auto hdr = ss::render_lit(*g, *p, cam, items, items);
        EXPECT_TRUE(hdr.has_value());
        if (!hdr) return 0.0F;
        const float lit = ss::luma(ss::mean_rgb(*hdr, cam, {-1.5F, 0.0F, 2.2F}));
        // The quad's shadow falls on x in [-2, 0].
        float sum = 0.0F;
        int n = 0;
        for (float x = -1.7F; x <= -0.3F; x += 0.35F) {
            for (float z = -0.7F; z <= 0.7F; z += 0.35F) {
                sum += ss::luma(ss::mean_rgb(*hdr, cam, {x, 0.0F, z}));
                ++n;
            }
        }
        return lit > 0.0F ? sum / static_cast<float>(n) / lit : 0.0F;
    };
    const float cutoff = transmitted(false);
    const float hashed = transmitted(true);
    EXPECT_GT(cutoff, 0.2F);
    EXPECT_LT(cutoff, 0.95F);
    EXPECT_NEAR(hashed, cutoff, 0.15F);
}
