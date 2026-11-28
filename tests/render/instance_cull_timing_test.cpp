// R9.4 shadow culling cost on this device, for the record (disabled by
// default; run with --gtest_also_run_disabled_tests). A mountain-forest-like
// field — 6000 trees (~2.3k triangles each) over 800 x 400 m plus 6000
// rocks and bushes from 0.2 to 1 m — rendered into the GBuffer and the four
// sun cascades. The casters are first culled on the CPU against
// caster_cull_view_proj (the frame loop's coarse volume), then drawn into
// every cascade (gpu_culling off, the pre-R9.4 path) or culled per cascade
// on the GPU, with and without the projected-size cull. Each configuration's
// graph is submitted and waited for; median of several runs. On lavapipe the
// GPU is the CPU, so wall-clock time is the device time: compare the figures
// relative to each other, not across devices.

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstdint>
#include <vector>

#include <glm/glm.hpp>
#include <glm/gtc/matrix_transform.hpp>
#include <gtest/gtest.h>
#include <vulkan/vulkan.h>

#include "engine/renderer/csm.hpp"
#include "engine/renderer/draw_batching.hpp"
#include "engine/renderer/instance_cull.hpp"
#include "engine/renderer/mesh_pass.hpp"
#include "engine/renderer/shadow_pass.hpp"
#include "engine/rhi/render_graph.hpp"
#include "render/occlusion_fixtures.hpp"
#include "support/expect_ok.hpp"
#include "support/gpu_context.hpp"
#include "support/gpu_submit.hpp"
#include "support/render_readback.hpp"

namespace {

namespace r = engine::renderer;
namespace rhi = engine::rhi;
namespace asset = engine::asset;
namespace occ = quat::test::occlusion;

constexpr VkExtent2D k_extent{1280, 720};
constexpr int        k_runs = 5;

// A UV sphere of radius 0.5 (rings x segments quads), with bounds.
std::expected<asset::MeshAsset, engine::core::Error> sphere(quat::test::GpuContext& g, int rings,
                                                            int segments) {
    std::vector<asset::Vertex>  verts;
    std::vector<std::uint32_t>  indices;
    for (int i = 0; i <= rings; ++i) {
        const float v = static_cast<float>(i) / static_cast<float>(rings);
        const float phi = v * 3.14159265F;
        for (int j = 0; j <= segments; ++j) {
            const float u = static_cast<float>(j) / static_cast<float>(segments);
            const float theta = u * 6.2831853F;
            const glm::vec3 n(std::sin(phi) * std::cos(theta), std::cos(phi),
                              std::sin(phi) * std::sin(theta));
            asset::Vertex vert;
            vert.position = 0.5F * n;
            vert.normal = n;
            vert.uv = {u, v};
            vert.tangent = {-std::sin(theta), 0.0F, std::cos(theta), 1.0F};
            verts.push_back(vert);
        }
    }
    const auto stride = static_cast<std::uint32_t>(segments + 1);
    for (int i = 0; i < rings; ++i) {
        for (int j = 0; j < segments; ++j) {
            const auto a = static_cast<std::uint32_t>(i) * stride + static_cast<std::uint32_t>(j);
            for (std::uint32_t k : {a, a + stride, a + 1, a + 1, a + stride, a + stride + 1}) {
                indices.push_back(k);
            }
        }
    }
    auto mesh = quat::test::upload_mesh(g, verts, indices);
    if (mesh) mesh->bounds = {glm::vec3(-0.5F), glm::vec3(0.5F)};
    return mesh;
}

} // namespace

TEST(GpuR94Timing, DISABLED_ShadowCullingOnThisDevice) {
    QUAT_REQUIRE_GPU(g);
    auto tree = sphere(*g, 24, 48);  // ~2.3k triangles
    ASSERT_OK(tree);
    auto pebble = sphere(*g, 8, 16); // ~260 triangles
    ASSERT_OK(pebble);
    auto floor = occ::upload_box(*g, {-500.0F, -0.5F, -500.0F}, {500.0F, 0.0F, 100.0F});
    ASSERT_OK(floor);
    auto grey = occ::upload_grey(*g);
    ASSERT_OK(grey);

    const occ::Camera cam = occ::look_at({0.0F, 2.0F, 0.0F}, {0.0F, 1.0F, -30.0F}, 60.0F,
                                         16.0F / 9.0F, 0.1F, 1000.0F);
    const glm::vec3 to_light = glm::normalize(glm::vec3(-0.5F, 0.8F, 0.3F));
    const r::CsmView view{cam.view, cam.proj, to_light};

    // The field, in world space (trees stretched to 2 x 9 x 2 m crowns).
    std::uint32_t seed = 777U;
    const auto rnd = [&seed]() {
        seed = seed * 1664525U + 1013904223U;
        return static_cast<float>(seed >> 8) / 16777216.0F;
    };
    std::vector<glm::mat4> trees;
    std::vector<glm::mat4> small;
    for (int i = 0; i < 6000; ++i) {
        const glm::vec3 p(rnd() * 800.0F - 400.0F, 4.5F, -rnd() * 400.0F + 20.0F);
        const float s = 0.7F + rnd() * 0.6F;
        trees.push_back(glm::scale(glm::translate(glm::mat4(1.0F), p),
                                   glm::vec3(2.0F * s, 9.0F * s, 2.0F * s)));
        const glm::vec3 q(rnd() * 800.0F - 400.0F, 0.1F, -rnd() * 400.0F + 20.0F);
        const float t = 0.2F + rnd() * 0.8F;
        small.push_back(glm::scale(glm::translate(glm::mat4(1.0F), q), glm::vec3(t)));
    }

    // The frame loop's lists: GBuffer instances culled against the camera,
    // casters against the coarse caster volume.
    const auto build = [&](const glm::mat4& cull_view_proj) {
        const r::ResolvedCullView v =
            r::resolve_cull_view(r::camera_cull_view(cull_view_proj), r::GpuCascades{});
        r::InstancedDrawList list;
        r::InstancedDraw floor_draw;
        floor_draw.mesh = &*floor;
        floor_draw.material = &*grey;
        floor_draw.instance_count = 1;
        list.instances.push_back(r::make_instance_data(glm::mat4(1.0F)));
        list.draws.push_back(floor_draw);
        for (const auto& [mesh, models] :
             {std::pair{&*tree, &trees}, std::pair{&*pebble, &small}}) {
            r::InstancedDraw d;
            d.mesh = mesh;
            d.material = &*grey;
            d.first_instance = static_cast<std::uint32_t>(list.instances.size());
            r::CullDrawGpu cd;
            cd.local_sphere = {0.0F, 0.0F, 0.0F, std::sqrt(3.0F) * 0.5F};
            for (const glm::mat4& m : *models) {
                if (!r::cull_accepts(v, cd, r::world_sphere(m, cd.local_sphere), cam.position)) {
                    continue;
                }
                list.instances.push_back(r::make_instance_data(m));
            }
            d.instance_count = static_cast<std::uint32_t>(list.instances.size()) - d.first_instance;
            list.draws.push_back(d);
        }
        return list;
    };
    const r::InstancedDrawList opaque = build(cam.view_proj);
    const r::CsmSettings base_csm{};
    const r::InstancedDrawList casters =
        build(r::caster_cull_view_proj(view, base_csm, r::shadow_map_size));

    auto mesh = r::MeshPass::create(g->device, g->allocator, g->cache, g->transfer, g->shader_dir);
    ASSERT_OK(mesh);
    auto shadow = r::ShadowPass::create(g->device, g->allocator, g->cache, g->shader_dir);
    ASSERT_OK(shadow);

    rhi::TransientImagePool pool(g->device, g->allocator);
    const auto time_ms = [&](bool with_shadows, const r::CsmSettings& csm) {
        std::vector<double> samples;
        for (int run = 0; run <= k_runs; ++run) {
            pool.reset();
            rhi::RenderGraph graph(pool);
            auto gbuffer = mesh->add_to_graph(graph, k_extent, cam.view_proj, opaque);
            EXPECT_TRUE(gbuffer.has_value());
            if (with_shadows) {
                // CPU-fitted cascades over the default range (no Hi-Z here).
                r::CsmSettings s = csm;
                s.gpu_depth_bounds = false;
                EXPECT_TRUE(shadow->add_to_graph(graph, view, s, nullptr, casters).has_value());
            }
            EXPECT_TRUE(graph.compile().has_value());
            const auto start = std::chrono::steady_clock::now();
            EXPECT_TRUE(quat::test::submit_and_wait(g->device, [&](VkCommandBuffer cmd) {
                            graph.execute(cmd);
                        }).has_value());
            const auto end = std::chrono::steady_clock::now();
            if (run > 0) samples.push_back(std::chrono::duration<double, std::milli>(end - start).count());
        }
        std::sort(samples.begin(), samples.end());
        return samples[samples.size() / 2];
    };

    r::CsmSettings off;
    off.gpu_culling = false;
    r::CsmSettings frustum;
    frustum.caster_min_texels = 0.0F;
    const r::CsmSettings sized; // defaults: gpu_culling, 1 texel
    r::CsmSettings sized4;
    sized4.caster_min_texels = 4.0F;

    const double gbuffer = time_ms(false, off);
    const double unculled = time_ms(true, off);
    const double culled = time_ms(true, frustum);
    const double culled_sized = time_ms(true, sized);
    const double culled_sized4 = time_ms(true, sized4);
    std::printf("[r9.4 timing] %ux%u, %zu GBuffer instances, %zu casters after the CPU cull, "
                "median of %d runs (ms)\n",
                k_extent.width, k_extent.height, opaque.instances.size(),
                casters.instances.size(), k_runs);
    std::printf("  gbuffer (GPU-culled camera view)          %8.2f\n", gbuffer);
    std::printf("  4 cascades, every caster in every cascade %8.2f\n", unculled - gbuffer);
    std::printf("  4 cascades, GPU culled per cascade        %8.2f\n", culled - gbuffer);
    std::printf("  ... + projected size >= 1 texel (default) %8.2f\n", culled_sized - gbuffer);
    std::printf("  ... + projected size >= 4 texels          %8.2f\n", culled_sized4 - gbuffer);
    EXPECT_LT(culled, unculled);
}
