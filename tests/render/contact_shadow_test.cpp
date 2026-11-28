// Contact shadows (R12): a small box on the floor that the cascaded shadow
// map misses (it is not in the caster list) still shadows the floor beside
// it through the screen-space march, while open floor stays as lit as
// without the march.

#include <cstdint>
#include <expected>
#include <vector>

#include <glm/glm.hpp>
#include <gtest/gtest.h>
#include <vulkan/vulkan.h>

#include "engine/asset/material_asset.hpp"
#include "engine/asset/mesh_asset.hpp"
#include "engine/renderer/lighting_pass.hpp"
#include "engine/renderer/mesh_pass.hpp"
#include "engine/renderer/shadow_pass.hpp"
#include "engine/rhi/render_graph.hpp"
#include "render/occlusion_fixtures.hpp"
#include "support/expect_ok.hpp"
#include "support/gpu_context.hpp"
#include "support/render_readback.hpp"

namespace {

namespace r = engine::renderer;
namespace rhi = engine::rhi;
namespace asset = engine::asset;
namespace occ = quat::test::occlusion;
using engine::core::Error;

constexpr VkExtent2D k_extent{128, 128};
// Low sun from -x: the 10 cm box's shadow reaches ~17 cm along +x.
const glm::vec3 k_to_light = glm::normalize(glm::vec3(-1.0F, 0.6F, 0.0F));
const glm::vec3 k_beside(0.09F, 0.0F, 0.0F);     // in the box's shadow
const glm::vec3 k_sun_side(-0.15F, 0.0F, 0.0F);  // lit, sun side
const glm::vec3 k_open(-0.25F, 0.0F, 0.25F);     // lit, away from the box

occ::Camera camera() {
    return occ::look_at({0.0F, 0.45F, 0.6F}, {0.0F, 0.0F, 0.0F}, 60.0F, 1.0F, 0.05F, 50.0F);
}

struct Lit {
    quat::test::HostImage with_contact;
    quat::test::HostImage without_contact;
};

std::expected<Lit, Error> render(quat::test::GpuContext& g) {
    auto floor = occ::upload_box(g, {-5.0F, -0.5F, -5.0F}, {5.0F, 0.0F, 5.0F});
    auto pebble = occ::upload_box(g, {-0.05F, 0.0F, -0.05F}, {0.05F, 0.1F, 0.05F});
    auto grey = occ::upload_grey(g);
    if (!floor) return std::unexpected(floor.error());
    if (!pebble) return std::unexpected(pebble.error());
    if (!grey) return std::unexpected(grey.error());
    auto mesh = r::MeshPass::create(g.device, g.allocator, g.cache, g.transfer, g.shader_dir);
    auto shadow = r::ShadowPass::create(g.device, g.allocator, g.cache, g.shader_dir);
    auto lighting = r::LightingPass::create(g.device, g.allocator, g.cache, g.shader_dir);
    if (!mesh) return std::unexpected(mesh.error());
    if (!shadow) return std::unexpected(shadow.error());
    if (!lighting) return std::unexpected(lighting.error());

    const occ::Camera cam = camera();
    const std::vector<r::DrawItem> visible{{&*floor, &*grey, glm::mat4(1.0F)},
                                           {&*pebble, &*grey, glm::mat4(1.0F)}};
    const std::vector<r::DrawItem> casters{{&*floor, &*grey, glm::mat4(1.0F)}};
    r::CsmSettings csm;
    csm.gpu_depth_bounds = false;
    csm.max_distance = 20.0F;

    Lit out;
    for (int with = 1; with >= 0; --with) {
        rhi::TransientImagePool pool(g.device, g.allocator);
        rhi::RenderGraph graph(pool);
        auto gbuffer = mesh->add_to_graph(graph, k_extent, cam.view_proj, visible);
        if (!gbuffer) return std::unexpected(gbuffer.error());
        auto shadows = shadow->add_to_graph(graph, {cam.view, cam.proj, k_to_light}, csm, nullptr,
                                            casters);
        if (!shadows) return std::unexpected(shadows.error());
        r::DirectionalLightParams sun;
        sun.direction = glm::vec4(k_to_light, 0.0F);
        sun.color = {1.0F, 1.0F, 1.0F, 3.0F};
        sun.ambient = {0.02F, 0.02F, 0.02F, 0.0F};
        r::LightingOcclusion occlusion;
        occlusion.shadows = *shadows;
        occlusion.csm = csm;
        occlusion.projection = cam.proj;
        if (with == 1) occlusion.contact = r::ContactShadowSettings{};
        auto hdr = lighting->add_to_graph(graph, *gbuffer, k_extent, sun,
                                          glm::inverse(cam.view_proj), cam.position, occlusion);
        if (!hdr) return std::unexpected(hdr.error());
        auto img = quat::test::render_to_host(g, graph, *hdr, k_extent, 8);
        if (!img) return std::unexpected(img.error());
        (with == 1 ? out.with_contact : out.without_contact) = std::move(*img);
    }
    return out;
}

const std::expected<Lit, Error>& lit(quat::test::GpuContext& g) {
    static const std::expected<Lit, Error> r = render(g);
    return r;
}

float red_at(const quat::test::HostImage& hdr, const glm::vec3& p) {
    const glm::uvec2 px = occ::project(camera().view_proj, p, k_extent);
    return hdr.rgba16f(px.x, px.y).r;
}

} // namespace

TEST(GpuContactShadows, ShadowMapMissesTheSmallObject) {
    QUAT_REQUIRE_GPU(g);
    const auto& img = lit(*g);
    ASSERT_OK(img);
    // Without the march the floor beside the pebble is as lit as the sun side.
    const float beside = red_at(img->without_contact, k_beside);
    const float sun_side = red_at(img->without_contact, k_sun_side);
    EXPECT_GT(sun_side, 0.2F);
    EXPECT_GT(beside, 0.9F * sun_side);
}

TEST(GpuContactShadows, MarchShadowsTheFloorBesideIt) {
    QUAT_REQUIRE_GPU(g);
    const auto& img = lit(*g);
    ASSERT_OK(img);
    const float beside = red_at(img->with_contact, k_beside);
    const float sun_side = red_at(img->with_contact, k_sun_side);
    EXPECT_LT(beside, 0.4F * sun_side);
}

TEST(GpuContactShadows, OpenFloorIsNotSelfShadowed) {
    QUAT_REQUIRE_GPU(g);
    const auto& img = lit(*g);
    ASSERT_OK(img);
    for (const glm::vec3& p : {k_sun_side, k_open}) {
        const float with = red_at(img->with_contact, p);
        const float without = red_at(img->without_contact, p);
        EXPECT_NEAR(with, without, 0.03F * without);
    }
}

// A long floor seen at a grazing angle under a low sun (the march's worst
// case for re-hitting its own surface) is not darkened by the march.
TEST(GpuContactShadows, GrazingFloorIsNotSelfShadowed) {
    QUAT_REQUIRE_GPU(g);
    auto floor = occ::upload_box(*g, {-50.0F, -0.5F, -200.0F}, {50.0F, 0.0F, 5.0F});
    auto grey = occ::upload_grey(*g);
    ASSERT_OK(floor);
    ASSERT_OK(grey);
    auto mesh = r::MeshPass::create(g->device, g->allocator, g->cache, g->transfer, g->shader_dir);
    auto lighting = r::LightingPass::create(g->device, g->allocator, g->cache, g->shader_dir);
    ASSERT_OK(mesh);
    ASSERT_OK(lighting);
    const occ::Camera cam =
        occ::look_at({0.0F, 1.7F, 0.0F}, {0.0F, 1.2F, -50.0F}, 60.0F, 1.0F, 0.1F, 300.0F);
    const std::vector<r::DrawItem> draws{{&*floor, &*grey, glm::mat4(1.0F)}};
    std::vector<quat::test::HostImage> images;
    for (bool with : {true, false}) {
        rhi::TransientImagePool pool(g->device, g->allocator);
        rhi::RenderGraph graph(pool);
        auto gbuffer = mesh->add_to_graph(graph, k_extent, cam.view_proj, draws);
        ASSERT_OK(gbuffer);
        r::DirectionalLightParams sun;
        sun.direction = glm::vec4(glm::normalize(glm::vec3(0.3F, 0.27F, -1.0F)), 0.0F);
        sun.color = {1.0F, 1.0F, 1.0F, 3.0F};
        r::LightingOcclusion occlusion;
        occlusion.projection = cam.proj;
        if (with) occlusion.contact = r::ContactShadowSettings{};
        auto hdr = lighting->add_to_graph(graph, *gbuffer, k_extent, sun,
                                          glm::inverse(cam.view_proj), cam.position, occlusion);
        ASSERT_OK(hdr);
        auto img = quat::test::render_to_host(*g, graph, *hdr, k_extent, 8);
        ASSERT_OK(img);
        images.push_back(std::move(*img));
    }
    // Every floor pixel (lower half of the image).
    for (std::uint32_t y = k_extent.height / 2 + 4; y < k_extent.height; y += 3) {
        for (std::uint32_t x = 0; x < k_extent.width; x += 3) {
            const float with = images[0].rgba16f(x, y).r;
            const float without = images[1].rgba16f(x, y).r;
            ASSERT_NEAR(with, without, 0.03F * without) << "pixel " << x << "," << y;
        }
    }
}
