// Translucent shadows (R12.11, issue #23): clear glass (transmission = 1)
// lets most of the sun through instead of casting a black shadow, tinted
// glass colours it, an alpha-blended caster lets (1 - alpha) through, and
// the same quad as an opaque material still shadows fully, and a closed
// absorbing glass slab absorbs once per volume (not once per face). The glass
// floats 1 m above a white floor with the sun at 45 degrees; it is drawn into
// the shadow pass only (glass renders in the forward pass, not the GBuffer).

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <string>
#include <vector>

#include <glm/glm.hpp>
#include <glm/gtc/matrix_transform.hpp>
#include <gtest/gtest.h>

#include "engine/asset/material_asset.hpp"
#include "engine/scene/gltf_loader.hpp"
#include "engine/scene/primitives.hpp"
#include "render/shadow_scene_fixture.hpp"
#include "support/expect_ok.hpp"
#include "support/gpu_context.hpp"

namespace {

namespace a = engine::asset;
namespace r = engine::renderer;
namespace occ = quat::test::occlusion;
namespace ss = quat::test::shadow_scene;

// Floor points under the quad's shadow and well outside it.
const glm::vec3 k_shadowed(-1.0F, 0.0F, 0.0F);
const glm::vec3 k_open(-1.0F, 0.0F, 2.6F);

struct Result {
    glm::vec3 shadowed{0.0F};
    glm::vec3 open{0.0F};
};

enum class Caster : std::uint8_t {
    quad, // a 3 m x 3 m quad at y = 1
    slab, // a closed 3 m x 0.5 m x 3 m box centred at y = 1
};

// Renders the floor plus a caster of material `caster`.
Result render(quat::test::GpuContext& g, const a::PbrMaterialParams& caster,
              Caster shape = Caster::quad) {
    Result out;
    auto p = ss::make_passes(g);
    EXPECT_OK(p);
    if (!p) return out;
    auto floor = occ::upload_box(g, {-8.0F, -0.5F, -8.0F}, {8.0F, 0.0F, 8.0F});
    auto quad = shape == Caster::quad
                    ? ss::upload_quad(g, 1.0F, 1.5F)
                    : occ::upload_box(g, {-1.5F, 0.75F, -1.5F}, {1.5F, 1.25F, 1.5F});
    auto white = occ::upload_grey(g, 0.8F);
    auto mat = ss::upload_params(g, caster);
    EXPECT_OK(floor);
    EXPECT_OK(quad);
    EXPECT_OK(white);
    EXPECT_OK(mat);
    if (!floor || !quad || !white || !mat) return out;
    const r::DrawItem floor_item{&*floor, &*white, glm::mat4(1.0F)};
    const r::DrawItem quad_item{&*quad, &*mat, glm::mat4(1.0F)};
    const bool forward = (caster.flags & (a::material_blend | a::material_transmission)) != 0;
    std::vector<r::DrawItem> gbuffer{floor_item};
    if (!forward) gbuffer.push_back(quad_item);
    const std::array<r::DrawItem, 2> casters{floor_item, quad_item};
    const occ::Camera cam = ss::shadow_camera();
    auto hdr = ss::render_lit(g, *p, cam, gbuffer, casters);
    EXPECT_OK(hdr);
    if (!hdr) return out;
    out.shadowed = ss::mean_rgb(*hdr, cam, k_shadowed);
    out.open = ss::mean_rgb(*hdr, cam, k_open);
    return out;
}

a::PbrMaterialParams glass(glm::vec3 tint) {
    a::PbrMaterialParams p;
    p.base_color_factor = glm::vec4(tint, 1.0F);
    p.metallic_factor = 0.0F;
    p.roughness_factor = 0.05F;
    p.transmission_factor = 1.0F;
    p.ior = 1.5F;
    p.flags = a::material_transmission;
    return p;
}

TEST(GpuTranslucentShadow, ClearGlassCastsAFaintShadow) {
    QUAT_REQUIRE_GPU(g);
    const Result r = render(*g, glass(glm::vec3(1.0F)));
    const float open = ss::luma(r.open);
    ASSERT_GT(open, 0.1F);
    // One air-glass interface reflects ~9 % on average (the shadow uses the
    // hemispherical average Fresnel, issue #77): the sun mostly passes. The
    // issue's bar is >= 80 % of the unshadowed value.
    EXPECT_GT(ss::luma(r.shadowed), 0.8F * open);
    EXPECT_LT(ss::luma(r.shadowed), 1.0F * open); // but there is a shadow
}

TEST(GpuTranslucentShadow, TintedGlassColoursTheShadow) {
    QUAT_REQUIRE_GPU(g);
    const Result r = render(*g, glass(glm::vec3(1.0F, 0.25F, 0.25F)));
    ASSERT_GT(r.open.r, 0.1F);
    // Red passes, green / blue are mostly absorbed (the ambient remains).
    EXPECT_GT(r.shadowed.r, 0.8F * r.open.r);
    EXPECT_LT(r.shadowed.g, 0.45F * r.open.g);
    EXPECT_GT(r.shadowed.r, 2.0F * r.shadowed.g);
}

TEST(GpuTranslucentShadow, BlendedCasterLetsOneMinusAlphaThrough) {
    QUAT_REQUIRE_GPU(g);
    a::PbrMaterialParams p;
    p.base_color_factor = {0.2F, 0.2F, 0.2F, 0.5F};
    p.metallic_factor = 0.0F;
    p.roughness_factor = 1.0F;
    p.flags = a::material_blend;
    const Result r = render(*g, p);
    const float open = ss::luma(r.open);
    ASSERT_GT(open, 0.1F);
    const float ratio = ss::luma(r.shadowed) / open;
    EXPECT_GT(ratio, 0.4F);
    EXPECT_LT(ratio, 0.65F);
}

// The opaque counterpart still casts a full shadow: only the ambient is left.
TEST(GpuTranslucentShadow, OpaqueCasterStillShadowsFully) {
    QUAT_REQUIRE_GPU(g);
    a::PbrMaterialParams p;
    p.base_color_factor = {1.0F, 1.0F, 1.0F, 1.0F};
    p.metallic_factor = 0.0F;
    p.roughness_factor = 1.0F;
    const Result r = render(*g, p);
    const float open = ss::luma(r.open);
    ASSERT_GT(open, 0.1F);
    EXPECT_LT(ss::luma(r.shadowed), 0.2F * open);
}

// KHR_materials_volume: light crossing the 0.5 m slab is absorbed over
// 0.5 m once (attenuation colour (1, .25, .25) at 1 m: green / blue halve),
// on top of two air-glass interfaces (~0.83 together with the average
// Fresnel). Absorbing on both faces would square it (~0.26 of the open green).
TEST(GpuTranslucentShadow, VolumeAbsorbsOncePerVolume) {
    QUAT_REQUIRE_GPU(g);
    a::PbrMaterialParams p = glass(glm::vec3(1.0F));
    p.attenuation = {1.0F, 0.25F, 0.25F, 1.0F};
    p.thickness = 0.5F;
    const Result r = render(*g, p, Caster::slab);
    ASSERT_GT(r.open.g, 0.1F);
    const float red = r.shadowed.r / r.open.r;
    const float green = r.shadowed.g / r.open.g;
    EXPECT_GT(red, 0.8F) << "red is not absorbed";
    EXPECT_GT(green, 0.4F) << "absorbed more than once";
    EXPECT_LT(green, 0.6F) << "not absorbed";
}

// Issue #77: a glass sphere's shadow had a 1-2 px stair-stepped rim darker
// than both its tinted interior and the lit floor. Along radial lines from the
// centre of the (elliptical) shadow out across its edge, every floor pixel
// must stay at or above the darker of the two.
TEST(GpuTranslucentShadow, GlassSphereShadowHasNoDarkRim) {
    QUAT_REQUIRE_GPU(g);
    auto p = ss::make_passes(*g);
    ASSERT_OK(p);
    auto floor = occ::upload_box(*g, {-8.0F, -0.5F, -8.0F}, {8.0F, 0.0F, 8.0F});
    auto sphere = engine::scene::upload_mesh(engine::scene::make_sphere_data(1.0F, 48, 64),
                                             g->allocator, g->transfer);
    auto white = occ::upload_grey(*g, 0.8F);
    auto mat = ss::upload_params(*g, glass(glm::vec3(0.85F, 0.92F, 1.0F)));
    ASSERT_OK(floor);
    ASSERT_OK(sphere);
    ASSERT_OK(white);
    ASSERT_OK(mat);
    const glm::vec3 centre(0.0F, 1.5F, 0.0F);
    const r::DrawItem floor_item{&*floor, &*white, glm::mat4(1.0F)};
    const r::DrawItem sphere_item{&*sphere, &*mat, glm::translate(glm::mat4(1.0F), centre)};
    const std::array<r::DrawItem, 1> gbuffer{floor_item};
    const std::array<r::DrawItem, 2> casters{floor_item, sphere_item};
    // The shadow: an ellipse centred under the sphere along the light, with
    // half-axes r / sin(45) along x and r along z.
    const glm::vec3 shadow_centre(-centre.y, 0.0F, 0.0F);
    const glm::vec2 half_axes(std::sqrt(2.0F), 1.0F);
    const occ::Camera cam =
        occ::look_at(shadow_centre + glm::vec3(0.0F, 5.0F, 1.5F), shadow_centre, 50.0F);
    auto hdr = ss::render_lit(*g, *p, cam, gbuffer, casters);
    ASSERT_OK(hdr);

    const auto luma_at = [&](const glm::vec3& w) {
        const glm::uvec2 px = occ::project(cam.view_proj, w, ss::k_extent);
        return ss::luma(glm::vec3(hdr->rgba16f(px.x, px.y)));
    };
    const float interior = ss::luma(ss::mean_rgb(*hdr, cam, shadow_centre));
    const float lit = ss::luma(ss::mean_rgb(*hdr, cam, shadow_centre + glm::vec3(0, 0, 2.0F)));
    ASSERT_GT(lit, 0.1F);
    ASSERT_LT(interior, 0.97F * lit) << "no tinted shadow under the sphere";
    const float floor_level = std::min(interior, lit) - 0.03F * lit;
    float darkest = lit;
    for (int a = 0; a < 16; ++a) {
        const float theta = static_cast<float>(a) * 0.392699F; // 22.5 degree steps
        const glm::vec2 dir(std::cos(theta) * half_axes.x, std::sin(theta) * half_axes.y);
        for (int k = 0; k <= 80; ++k) {
            const float s = 0.5F + 0.01F * static_cast<float>(k); // across the edge (s = 1)
            const glm::vec3 w = shadow_centre + glm::vec3(dir.x * s, 0.0F, dir.y * s);
            const float l = luma_at(w);
            darkest = std::min(darkest, l);
            EXPECT_GE(l, floor_level) << "dark rim at angle " << a * 22.5F << " s " << s;
        }
    }
    RecordProperty("darkest_over_floor_level", std::to_string(darkest / floor_level));
}

} // namespace
