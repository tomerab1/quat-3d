// Translucent shadows (R12.11, issue #23): clear glass (transmission = 1)
// lets most of the sun through instead of casting a black shadow, tinted
// glass colours it, an alpha-blended caster lets (1 - alpha) through, and
// the same quad as an opaque material still shadows fully. The glass quad
// floats 1 m above a white floor with the sun at 45 degrees; it is drawn into
// the shadow pass only (glass renders in the forward pass, not the GBuffer).

#include <array>
#include <vector>

#include <glm/glm.hpp>
#include <gtest/gtest.h>

#include "engine/asset/material_asset.hpp"
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

// Renders the floor plus a 1.5 m-half-size quad of material `caster`.
Result render(quat::test::GpuContext& g, const a::PbrMaterialParams& caster) {
    Result out;
    auto p = ss::make_passes(g);
    EXPECT_OK(p);
    if (!p) return out;
    auto floor = occ::upload_box(g, {-8.0F, -0.5F, -8.0F}, {8.0F, 0.0F, 8.0F});
    auto quad = ss::upload_quad(g, 1.0F, 1.5F);
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
    // One air-glass interface at 45 degrees reflects ~4 %: the sun mostly
    // passes. The issue's bar is >= 80 % of the unshadowed value.
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

} // namespace
