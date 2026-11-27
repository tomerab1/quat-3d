// Terrain as a sun-shadow caster (R12): a flat terrain tile raised 3 m above
// a mesh floor acts as a roof; with the terrain's ShadowCasterFn the floor
// beneath it is shadowed, and without it the floor stays lit.

#include <cstdint>
#include <expected>
#include <vector>

#include <glm/glm.hpp>
#include <gtest/gtest.h>
#include <vulkan/vulkan.h>

#include "engine/renderer/lighting_pass.hpp"
#include "engine/renderer/mesh_pass.hpp"
#include "engine/renderer/shadow_pass.hpp"
#include "engine/renderer/terrain_pass.hpp"
#include "engine/rhi/render_graph.hpp"
#include "engine/terrain/heightmap.hpp"
#include "render/occlusion_fixtures.hpp"
#include "support/expect_ok.hpp"
#include "support/gpu_context.hpp"
#include "support/render_readback.hpp"

namespace {

namespace r = engine::renderer;
namespace rhi = engine::rhi;
namespace occ = quat::test::occlusion;
using engine::core::Error;

constexpr VkExtent2D k_extent{128, 128};
const glm::vec3      k_anchor(0.0F, 3.0F, 0.0F); // the tile's surface height
const glm::vec3      k_to_light = glm::normalize(glm::vec3(0.1F, 1.0F, 0.1F));
const glm::vec3      k_under_roof(0.0F, 0.0F, 6.0F);
const glm::vec3      k_open_sky(0.0F, 0.0F, 22.0F);

occ::Camera camera() {
    return occ::look_at({0.0F, 1.2F, 32.0F}, {0.0F, 0.0F, 6.0F}, 60.0F, 1.0F, 0.1F, 200.0F);
}

struct Pair {
    quat::test::HostImage with_terrain;
    quat::test::HostImage without_terrain;
};

std::expected<Pair, Error> render(quat::test::GpuContext& g) {
    auto floor = occ::upload_box(g, {-40.0F, -0.5F, -40.0F}, {40.0F, 0.0F, 40.0F});
    auto grey = occ::upload_grey(g);
    if (!floor) return std::unexpected(floor.error());
    if (!grey) return std::unexpected(grey.error());
    auto mesh = r::MeshPass::create(g.device, g.allocator, g.cache, g.transfer, g.shader_dir);
    auto terrain = r::TerrainPass::create(g.device, g.allocator, g.cache, g.transfer, g.shader_dir);
    auto shadow = r::ShadowPass::create(g.device, g.allocator, g.cache, g.shader_dir);
    auto lighting = r::LightingPass::create(g.device, g.allocator, g.cache, g.shader_dir);
    if (!mesh) return std::unexpected(mesh.error());
    if (!terrain) return std::unexpected(terrain.error());
    if (!shadow) return std::unexpected(shadow.error());
    if (!lighting) return std::unexpected(lighting.error());

    // A flat 32 m tile: the roof spans x, z in [-16, 16] at y = 3.
    engine::terrain::Heightmap map;
    map.resolution = 65;
    map.tile_size_m = 32.0F;
    map.heights.assign(static_cast<std::size_t>(65) * 65, 0.0F);
    if (auto set = terrain->set_tile({0, 0}, map, g.transfer); !set) {
        return std::unexpected(set.error());
    }

    const occ::Camera cam = camera();
    const std::vector<r::DrawItem> draws{{&*floor, &*grey, glm::mat4(1.0F)}};
    r::CsmSettings csm;
    csm.gpu_depth_bounds = false;
    csm.max_distance = 60.0F;
    const r::CsmView view{cam.view, cam.proj, k_to_light};
    Pair out;
    for (int with = 1; with >= 0; --with) {
        rhi::TransientImagePool pool(g.device, g.allocator);
        rhi::RenderGraph graph(pool);
        auto gbuffer = mesh->add_to_graph(graph, k_extent, cam.view_proj, draws);
        if (!gbuffer) return std::unexpected(gbuffer.error());
        terrain->add_to_graph(graph, *gbuffer, k_extent, cam.view_proj, cam.position, k_anchor,
                              1000.0F);
        std::vector<r::ShadowCasterFn> extra;
        if (with == 1) {
            extra.push_back(terrain->shadow_casters(r::caster_cull_view_proj(view, csm,
                                                                             r::shadow_map_size),
                                                    cam.position, k_anchor));
            if (!extra.back()) return std::unexpected(Error{"terrain produced no casters"});
        }
        auto shadows = shadow->add_to_graph(graph, view, csm, nullptr, r::batch_draws(draws),
                                            extra);
        if (!shadows) return std::unexpected(shadows.error());
        r::DirectionalLightParams sun;
        sun.direction = glm::vec4(k_to_light, 0.0F);
        sun.color = {1.0F, 1.0F, 1.0F, 3.0F};
        sun.ambient = {0.02F, 0.02F, 0.02F, 0.0F};
        r::LightingOcclusion occlusion;
        occlusion.shadows = *shadows;
        occlusion.csm = csm;
        occlusion.projection = cam.proj;
        auto hdr = lighting->add_to_graph(graph, *gbuffer, k_extent, sun,
                                          glm::inverse(cam.view_proj), cam.position, occlusion);
        if (!hdr) return std::unexpected(hdr.error());
        auto img = quat::test::render_to_host(g, graph, *hdr, k_extent, 8);
        if (!img) return std::unexpected(img.error());
        (with == 1 ? out.with_terrain : out.without_terrain) = std::move(*img);
    }
    return out;
}

float red_at(const quat::test::HostImage& hdr, const glm::vec3& p) {
    const glm::uvec2 px = occ::project(camera().view_proj, p, k_extent);
    return hdr.rgba16f(px.x, px.y).r;
}

} // namespace

TEST(GpuTerrainShadow, TerrainRoofShadowsTheFloorBeneath) {
    QUAT_REQUIRE_GPU(g);
    const auto images = render(*g);
    ASSERT_OK(images);
    const float open = red_at(images->with_terrain, k_open_sky);
    EXPECT_GT(open, 0.2F);
    // Without the terrain caster nothing blocks the sun under the roof.
    EXPECT_GT(red_at(images->without_terrain, k_under_roof), 0.9F * open);
    EXPECT_LT(red_at(images->with_terrain, k_under_roof), 0.2F * open);
}
