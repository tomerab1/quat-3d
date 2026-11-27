// GTAO (R12): a concave floor/wall corner is occluded, an open plane is not,
// bent normals lean out of the corner, and the lighting pass applies the
// occlusion to the ambient term only where there is some.

#include <cstdint>
#include <expected>
#include <vector>

#include <glm/glm.hpp>
#include <gtest/gtest.h>
#include <vulkan/vulkan.h>

#include "engine/asset/material_asset.hpp"
#include "engine/asset/mesh_asset.hpp"
#include "engine/renderer/gtao_pass.hpp"
#include "engine/renderer/hiz_pass.hpp"
#include "engine/renderer/lighting_pass.hpp"
#include "engine/renderer/mesh_pass.hpp"
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

// A floor (top at y = 0) and a wall whose -x face stands at x = 2.
struct CornerScene {
    asset::MeshAsset     floor;
    asset::MeshAsset     wall;
    asset::MaterialAsset grey;
    occ::Camera          cam;

    [[nodiscard]] std::vector<r::DrawItem> draws() const {
        return {{&floor, &grey, glm::mat4(1.0F)}, {&wall, &grey, glm::mat4(1.0F)}};
    }
};

std::expected<CornerScene, Error> make_corner(quat::test::GpuContext& g) {
    CornerScene s;
    auto floor = occ::upload_box(g, {-10.0F, -0.5F, -10.0F}, {10.0F, 0.0F, 10.0F});
    if (!floor) return std::unexpected(floor.error());
    s.floor = std::move(*floor);
    auto wall = occ::upload_box(g, {2.0F, 0.0F, -10.0F}, {3.0F, 4.0F, 10.0F});
    if (!wall) return std::unexpected(wall.error());
    s.wall = std::move(*wall);
    auto grey = occ::upload_grey(g, 0.5F);
    if (!grey) return std::unexpected(grey.error());
    s.grey = std::move(*grey);
    s.cam = occ::look_at({-3.0F, 3.0F, 4.0F}, {1.5F, 0.0F, 0.0F}, 60.0F, 1.0F, 0.1F, 100.0F);
    return s;
}

const glm::vec3 k_corner_floor(1.9F, 0.0F, 0.0F);
const glm::vec3 k_corner_wall(2.0F, 0.1F, 0.0F);
const glm::vec3 k_open_floor(-1.0F, 0.0F, 0.0F); // 3 m from the wall

struct Images {
    quat::test::HostImage ao;      // RGBA8 GTAO output
    quat::test::HostImage lit;     // HDR with GTAO
    quat::test::HostImage unlit;   // HDR without GTAO
};

// Renders the corner's GBuffer once per variant: GTAO alone, and lighting
// (ambient only) with and without it.
std::expected<Images, Error> render(quat::test::GpuContext& g) {
    auto scene = make_corner(g);
    if (!scene) return std::unexpected(scene.error());
    auto mesh = r::MeshPass::create(g.device, g.allocator, g.cache, g.transfer, g.shader_dir);
    auto hiz = r::HiZPass::create(g.device, g.allocator, g.cache, g.shader_dir);
    auto gtao = r::GtaoPass::create(g.device, g.allocator, g.cache, g.shader_dir);
    auto lighting = r::LightingPass::create(g.device, g.allocator, g.cache, g.shader_dir);
    if (!mesh) return std::unexpected(mesh.error());
    if (!hiz) return std::unexpected(hiz.error());
    if (!gtao) return std::unexpected(gtao.error());
    if (!lighting) return std::unexpected(lighting.error());
    const std::vector<r::DrawItem> draws = scene->draws();
    const occ::Camera& cam = scene->cam;
    r::GtaoSettings settings;

    // 0: GTAO output, 1: lit with GTAO, 2: lit without.
    Images out;
    for (int variant = 0; variant < 3; ++variant) {
        rhi::TransientImagePool pool(g.device, g.allocator);
        rhi::RenderGraph graph(pool);
        auto gbuffer = mesh->add_to_graph(graph, k_extent, cam.view_proj, draws);
        if (!gbuffer) return std::unexpected(gbuffer.error());
        auto pyramid = hiz->add_to_graph(graph, gbuffer->depth, k_extent);
        if (!pyramid) return std::unexpected(pyramid.error());
        rhi::ResourceHandle ao{};
        if (variant < 2) {
            auto a = gtao->add_to_graph(graph, *pyramid, gbuffer->normal, k_extent, cam.view,
                                        cam.proj, settings, 0);
            if (!a) return std::unexpected(a.error());
            if (!a->valid()) return std::unexpected(Error{"gtao disabled unexpectedly"});
            ao = *a;
        }
        if (variant == 0) {
            auto img = quat::test::render_to_host(g, graph, ao, k_extent, 4);
            if (!img) return std::unexpected(img.error());
            out.ao = std::move(*img);
            continue;
        }
        r::DirectionalLightParams light;
        light.color = {1.0F, 1.0F, 1.0F, 0.0F}; // ambient only
        light.ambient = {1.0F, 1.0F, 1.0F, 0.0F};
        r::LightingOcclusion occlusion;
        occlusion.ao = ao;
        occlusion.gtao = settings;
        occlusion.projection = cam.proj;
        auto hdr = lighting->add_to_graph(graph, *gbuffer, k_extent, light,
                                          glm::inverse(cam.view_proj), cam.position, occlusion);
        if (!hdr) return std::unexpected(hdr.error());
        auto img = quat::test::render_to_host(g, graph, *hdr, k_extent, 8);
        if (!img) return std::unexpected(img.error());
        (variant == 1 ? out.lit : out.unlit) = std::move(*img);
    }
    return out;
}

const std::expected<Images, Error>& images(quat::test::GpuContext& g) {
    static const std::expected<Images, Error> r = render(g);
    return r;
}

float visibility_at(const Images& img, const occ::Camera& cam, const glm::vec3& p) {
    const glm::uvec2 px = occ::project(cam.view_proj, p, k_extent);
    return static_cast<float>(img.ao.rgba8(px.x, px.y).a) / 255.0F;
}

glm::vec3 bent_at(const Images& img, const occ::Camera& cam, const glm::vec3& p) {
    const glm::uvec2 px = occ::project(cam.view_proj, p, k_extent);
    const glm::ivec4 c = img.ao.rgba8(px.x, px.y);
    return glm::normalize(glm::vec3(c) / 255.0F * 2.0F - 1.0F);
}

float red_at(const quat::test::HostImage& hdr, const occ::Camera& cam, const glm::vec3& p) {
    const glm::uvec2 px = occ::project(cam.view_proj, p, k_extent);
    return hdr.rgba16f(px.x, px.y).r;
}

occ::Camera corner_camera() {
    return occ::look_at({-3.0F, 3.0F, 4.0F}, {1.5F, 0.0F, 0.0F}, 60.0F, 1.0F, 0.1F, 100.0F);
}

} // namespace

TEST(GpuGtao, CornerIsOccludedOpenPlaneIsNot) {
    QUAT_REQUIRE_GPU(g);
    const auto& img = images(*g);
    ASSERT_OK(img);
    const occ::Camera cam = corner_camera();
    EXPECT_LT(visibility_at(*img, cam, k_corner_floor), 0.75F);
    EXPECT_LT(visibility_at(*img, cam, k_corner_wall), 0.75F);
    EXPECT_GT(visibility_at(*img, cam, k_open_floor), 0.93F);
}

TEST(GpuGtao, BentNormalsLeanOutOfTheCorner) {
    QUAT_REQUIRE_GPU(g);
    const auto& img = images(*g);
    ASSERT_OK(img);
    const occ::Camera cam = corner_camera();
    const glm::vec3 open = bent_at(*img, cam, k_open_floor);
    EXPECT_GT(open.y, 0.9F); // an open floor's bent normal is its normal
    const glm::vec3 corner = bent_at(*img, cam, k_corner_floor);
    EXPECT_GT(corner.y, 0.3F);
    EXPECT_LT(corner.x, -0.1F); // away from the wall at +x
}

TEST(GpuGtao, LightingDarkensOnlyTheOccludedAmbient) {
    QUAT_REQUIRE_GPU(g);
    const auto& img = images(*g);
    ASSERT_OK(img);
    const occ::Camera cam = corner_camera();
    const float open_with = red_at(img->lit, cam, k_open_floor);
    const float open_without = red_at(img->unlit, cam, k_open_floor);
    EXPECT_GT(open_without, 0.1F);
    EXPECT_NEAR(open_with, open_without, 0.05F * open_without);
    const float corner_with = red_at(img->lit, cam, k_corner_floor);
    const float corner_without = red_at(img->unlit, cam, k_corner_floor);
    EXPECT_LT(corner_with, 0.85F * corner_without);
}
