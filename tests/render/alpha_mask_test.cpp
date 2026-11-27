// glTF alphaMode MASK on the GPU (R12.11, issue #35): a quad whose base
// colour alpha is a 2x2 checker of opaque / cut-out blocks is absent from the
// GBuffer where it is cut out and lets the sun through there in the cascaded
// shadow map; and the back face of a doubleSided material is lit with the
// reversed normal.

#include <array>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <vector>

#include <glm/glm.hpp>
#include <glm/gtc/packing.hpp>
#include <gtest/gtest.h>

#include "engine/asset/asset_manager.hpp"
#include "engine/asset/material_asset.hpp"
#include "engine/scene/gltf_loader.hpp"
#include "render/shadow_scene_fixture.hpp"
#include "support/expect_ok.hpp"
#include "support/gpu_context.hpp"

namespace {

namespace a = engine::asset;
namespace r = engine::renderer;
namespace occ = quat::test::occlusion;
namespace ss = quat::test::shadow_scene;

constexpr std::uint32_t k_tex = 64;

// White texture whose alpha is opaque in the (u < .5, v < .5) and
// (u >= .5, v >= .5) blocks and zero in the other two.
engine::scene::TextureData checker_alpha() {
    engine::scene::TextureData t;
    t.width = k_tex;
    t.height = k_tex;
    t.color_space = a::TextureColorSpace::srgb;
    t.alpha_test_cutoff = 0.5F; // coverage-preserving mips (uploaded level by level)
    t.pixels.resize(static_cast<std::size_t>(k_tex) * k_tex * 4, std::byte{255});
    for (std::uint32_t y = 0; y < k_tex; ++y) {
        for (std::uint32_t x = 0; x < k_tex; ++x) {
            const bool opaque = (x < k_tex / 2) == (y < k_tex / 2);
            t.pixels[(static_cast<std::size_t>(y) * k_tex + x) * 4 + 3] =
                opaque ? std::byte{255} : std::byte{0};
        }
    }
    return t;
}

struct MaskScene {
    a::AssetManager   assets;
    a::MeshAsset      floor;
    a::MeshAsset      quad;
    a::MaterialAsset  floor_mat;
    a::MaterialAsset  quad_mat;
};

// The floor (red) and the masked quad (green) 1 m above it, spanning
// x, z in [-1, 1]: blocks (x < 0, z < 0) and (x > 0, z > 0) are opaque.
void make_scene(quat::test::GpuContext& g, MaskScene& s, bool masked) {
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

    const auto texture = s.assets.load<a::TextureAsset>("test/checker", [&](const auto&) {
        return engine::scene::upload_texture(checker_alpha(), g.allocator, g.transfer);
    });
    ASSERT_TRUE(texture.is_loaded());
    a::PbrMaterialParams green = red;
    green.base_color_factor = {0.1F, 0.8F, 0.1F, 1.0F};
    green.alpha_cutoff = 0.5F;
    green.flags = a::material_has_base_color | (masked ? a::material_alpha_mask : 0U);
    a::MaterialTextures textures;
    textures.base_color = texture;
    auto quad_mat = ss::upload_params(g, green, textures);
    ASSERT_OK(quad_mat);
    s.quad_mat = std::move(*quad_mat);
}

std::vector<r::DrawItem> draws(const MaskScene& s) {
    return {{&s.floor, &s.floor_mat, glm::mat4(1.0F)}, {&s.quad, &s.quad_mat, glm::mat4(1.0F)}};
}

TEST(GpuAlphaMask, CutOutIsAbsentFromTheGBuffer) {
    QUAT_REQUIRE_GPU(g);
    MaskScene s;
    make_scene(*g, s, /*masked=*/true);
    auto p = ss::make_passes(*g);
    ASSERT_OK(p);
    const occ::Camera cam = occ::look_at({0.0F, 6.0F, 0.5F}, {0.0F, 0.0F, 0.0F});
    const auto items = draws(s);
    auto albedo = ss::render_albedo(*g, *p, cam, items);
    ASSERT_OK(albedo);
    const auto at = [&](glm::vec3 w) {
        const glm::uvec2 px = occ::project(cam.view_proj, w, ss::k_extent);
        return albedo->rgba8(px.x, px.y);
    };
    // Opaque blocks: the quad (green).
    for (const glm::vec3 w : {glm::vec3(-0.5F, 1.0F, -0.5F), glm::vec3(0.5F, 1.0F, 0.5F)}) {
        const glm::ivec4 c = at(w);
        EXPECT_GT(c.g, 3 * c.r) << "opaque block at " << w.x << ", " << w.z;
    }
    // Cut-out blocks: the floor below shows through (red).
    for (const glm::vec3 w : {glm::vec3(0.5F, 1.0F, -0.5F), glm::vec3(-0.5F, 1.0F, 0.5F)}) {
        const glm::ivec4 c = at(w);
        EXPECT_GT(c.r, 3 * c.g) << "cut-out block at " << w.x << ", " << w.z;
    }
}

// The same quad as an opaque material (the flag off) covers everything: the
// alpha only matters through the MASK pipeline.
TEST(GpuAlphaMask, OpaqueMaterialIgnoresAlpha) {
    QUAT_REQUIRE_GPU(g);
    MaskScene s;
    make_scene(*g, s, /*masked=*/false);
    auto p = ss::make_passes(*g);
    ASSERT_OK(p);
    const occ::Camera cam = occ::look_at({0.0F, 6.0F, 0.5F}, {0.0F, 0.0F, 0.0F});
    const auto items = draws(s);
    auto albedo = ss::render_albedo(*g, *p, cam, items);
    ASSERT_OK(albedo);
    const glm::uvec2 px = occ::project(cam.view_proj, {0.5F, 1.0F, -0.5F}, ss::k_extent);
    const glm::ivec4 c = albedo->rgba8(px.x, px.y);
    EXPECT_GT(c.g, 3 * c.r);
}

// The quad's shadow falls 1 m toward -x: under its opaque blocks the floor is
// shadowed, under the cut-outs the sun reaches it.
TEST(GpuAlphaMask, CutOutPassesLightInTheShadowMap) {
    QUAT_REQUIRE_GPU(g);
    MaskScene s;
    make_scene(*g, s, /*masked=*/true);
    auto p = ss::make_passes(*g);
    ASSERT_OK(p);
    const occ::Camera cam = ss::shadow_camera();
    const auto items = draws(s);
    auto hdr = ss::render_lit(*g, *p, cam, items, items);
    ASSERT_OK(hdr);
    const float lit = ss::luma(ss::mean_rgb(*hdr, cam, {-1.5F, 0.0F, 2.2F}));
    ASSERT_GT(lit, 0.1F);
    // Shadows of the opaque blocks (quad x in [-1, 0] -> floor x in [-2, -1]).
    for (const glm::vec3 w : {glm::vec3(-1.5F, 0.0F, -0.5F), glm::vec3(-0.5F, 0.0F, 0.5F)}) {
        EXPECT_LT(ss::luma(ss::mean_rgb(*hdr, cam, w)), 0.3F * lit)
            << "under opaque block, floor at " << w.x << ", " << w.z;
    }
    // Light through the cut-outs.
    for (const glm::vec3 w : {glm::vec3(-1.5F, 0.0F, 0.5F), glm::vec3(-0.5F, 0.0F, -0.5F)}) {
        EXPECT_GT(ss::luma(ss::mean_rgb(*hdr, cam, w)), 0.85F * lit)
            << "under cut-out, floor at " << w.x << ", " << w.z;
    }
}

// glTF doubleSided: the back face is shaded with the reversed normal; a
// single-sided material keeps its geometric normal (unchanged behaviour).
TEST(GpuAlphaMask, DoubleSidedBackFaceFacesTheViewer) {
    QUAT_REQUIRE_GPU(g);
    auto p = ss::make_passes(*g);
    ASSERT_OK(p);
    auto quad = ss::upload_quad(*g, 1.0F, 2.0F);
    ASSERT_OK(quad);
    // Below the quad, looking up at its back face.
    const occ::Camera cam = occ::look_at({0.0F, -3.0F, 0.5F}, {0.0F, 1.0F, 0.0F});
    for (const bool double_sided : {true, false}) {
        a::PbrMaterialParams params;
        params.roughness_factor = 1.0F;
        params.metallic_factor = 0.0F;
        params.flags = double_sided ? a::material_double_sided : 0U;
        auto mat = ss::upload_params(*g, params);
        ASSERT_OK(mat);
        const std::array<r::DrawItem, 1> items{{{&*quad, &*mat, glm::mat4(1.0F)}}};
        engine::rhi::TransientImagePool pool(g->device, g->allocator);
        engine::rhi::RenderGraph graph(pool);
        auto gbuffer = p->mesh.add_to_graph(graph, ss::k_extent, cam.view_proj, items);
        ASSERT_OK(gbuffer);
        auto normal = quat::test::render_to_host(*g, graph, gbuffer->normal, ss::k_extent, 4);
        ASSERT_OK(normal);
        // Decode the RG16F octahedral normal (gbuffer.slang oct_encode).
        const glm::uvec2 c = normal->centre();
        std::uint16_t h[2];
        std::memcpy(h, normal->at(c.x, c.y), sizeof(h));
        const glm::vec2 e(glm::unpackHalf1x16(h[0]), glm::unpackHalf1x16(h[1]));
        glm::vec3 n(e.x, e.y, 1.0F - std::abs(e.x) - std::abs(e.y));
        if (n.z < 0.0F) {
            const glm::vec2 xy(n.x, n.y);
            n.x = (1.0F - std::abs(xy.y)) * (xy.x >= 0.0F ? 1.0F : -1.0F);
            n.y = (1.0F - std::abs(xy.x)) * (xy.y >= 0.0F ? 1.0F : -1.0F);
        }
        n = glm::normalize(n);
        if (double_sided) {
            EXPECT_LT(n.y, -0.9F) << "back face of a doubleSided quad must face down";
        } else {
            EXPECT_GT(n.y, 0.9F) << "single-sided quad keeps its +y normal";
        }
    }
}

} // namespace
