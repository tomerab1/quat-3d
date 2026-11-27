// R12 pass costs on this device, for the record (disabled by default; run
// with --gtest_also_run_disabled_tests). Each configuration's whole graph is
// submitted and waited for; per-feature cost is the difference to the
// configuration without it (median of several runs). On lavapipe the GPU is
// the CPU, so wall-clock time is the device time.

#include <algorithm>
#include <chrono>
#include <cstdio>
#include <cstdint>
#include <vector>

#include <glm/glm.hpp>
#include <gtest/gtest.h>
#include <vulkan/vulkan.h>

#include "engine/renderer/gtao_pass.hpp"
#include "engine/renderer/hiz_pass.hpp"
#include "engine/renderer/lighting_pass.hpp"
#include "engine/renderer/mesh_pass.hpp"
#include "engine/renderer/shadow_pass.hpp"
#include "engine/rhi/render_graph.hpp"
#include "render/occlusion_fixtures.hpp"
#include "support/expect_ok.hpp"
#include "support/gpu_context.hpp"
#include "support/gpu_submit.hpp"

namespace {

namespace r = engine::renderer;
namespace rhi = engine::rhi;
namespace asset = engine::asset;
namespace occ = quat::test::occlusion;

constexpr VkExtent2D k_extent{1280, 720};
constexpr int        k_runs = 5;

enum Feature : unsigned {
    f_hiz = 1U,
    f_csm = 2U,
    f_gtao = 4U,
    f_lighting = 8U,
    f_contact = 16U,
};

} // namespace

TEST(GpuR12Timing, DISABLED_PassTimesOnThisDevice) {
    QUAT_REQUIRE_GPU(g);
    auto floor = occ::upload_box(*g, {-60.0F, -0.5F, -120.0F}, {60.0F, 0.0F, 10.0F});
    ASSERT_OK(floor);
    std::vector<asset::MeshAsset> boxes;
    for (int i = 0; i < 48; ++i) {
        const glm::vec3 c(static_cast<float>(i % 8) * 4.0F - 14.0F, 1.0F,
                          -static_cast<float>(i / 8) * 6.0F - 4.0F);
        auto b = occ::upload_box(*g, c - glm::vec3(1.0F), c + glm::vec3(1.0F));
        ASSERT_OK(b);
        boxes.push_back(std::move(*b));
    }
    auto grey = occ::upload_grey(*g);
    ASSERT_OK(grey);
    std::vector<r::DrawItem> draws{{&*floor, &*grey, glm::mat4(1.0F)}};
    for (const asset::MeshAsset& b : boxes) draws.push_back({&b, &*grey, glm::mat4(1.0F)});

    auto mesh = r::MeshPass::create(g->device, g->allocator, g->cache, g->transfer, g->shader_dir);
    auto hiz = r::HiZPass::create(g->device, g->allocator, g->cache, g->shader_dir);
    auto shadow = r::ShadowPass::create(g->device, g->allocator, g->cache, g->shader_dir);
    auto gtao = r::GtaoPass::create(g->device, g->allocator, g->cache, g->shader_dir);
    auto lighting = r::LightingPass::create(g->device, g->allocator, g->cache, g->shader_dir);
    ASSERT_OK(mesh);
    ASSERT_OK(hiz);
    ASSERT_OK(shadow);
    ASSERT_OK(gtao);
    ASSERT_OK(lighting);
    const occ::Camera cam = occ::look_at({0.0F, 4.0F, 8.0F}, {0.0F, 0.0F, -20.0F}, 60.0F,
                                         16.0F / 9.0F, 0.1F, 300.0F);
    const glm::vec3 to_light = glm::normalize(glm::vec3(-0.5F, 1.0F, 0.3F));

    rhi::TransientImagePool pool(g->device, g->allocator);
    const auto time_ms = [&](unsigned features) {
        std::vector<double> samples;
        for (int run = 0; run <= k_runs; ++run) {
            pool.reset();
            rhi::RenderGraph graph(pool);
            auto gbuffer = mesh->add_to_graph(graph, k_extent, cam.view_proj, draws);
            EXPECT_TRUE(gbuffer.has_value());
            r::HiZTargets pyramid{};
            if ((features & f_hiz) != 0) pyramid = *hiz->add_to_graph(graph, gbuffer->depth,
                                                                      k_extent);
            r::LightingOcclusion occlusion;
            occlusion.projection = cam.proj;
            if ((features & f_csm) != 0) {
                occlusion.shadows = *shadow->add_to_graph(graph, {cam.view, cam.proj, to_light},
                                                          occlusion.csm, &pyramid, draws);
            }
            if ((features & f_gtao) != 0) {
                occlusion.ao = *gtao->add_to_graph(graph, pyramid, gbuffer->normal, k_extent,
                                                   cam.view, cam.proj, occlusion.gtao, 0);
            }
            if ((features & f_contact) != 0) occlusion.contact = r::ContactShadowSettings{};
            if ((features & f_lighting) != 0) {
                r::DirectionalLightParams sun;
                sun.direction = glm::vec4(to_light, 0.0F);
                EXPECT_TRUE(lighting
                                ->add_to_graph(graph, *gbuffer, k_extent, sun,
                                               glm::inverse(cam.view_proj), cam.position,
                                               occlusion)
                                .has_value());
            }
            EXPECT_TRUE(graph.compile().has_value());
            const auto start = std::chrono::steady_clock::now();
            EXPECT_TRUE(quat::test::submit_and_wait(g->device, [&](VkCommandBuffer cmd) {
                            graph.execute(cmd);
                        }).has_value());
            const auto end = std::chrono::steady_clock::now();
            if (run > 0) { // run 0 warms up (pipeline JIT, first-use allocations)
                samples.push_back(std::chrono::duration<double, std::milli>(end - start).count());
            }
        }
        std::sort(samples.begin(), samples.end());
        return samples[samples.size() / 2];
    };

    const double base = time_ms(0);
    const double with_hiz = time_ms(f_hiz);
    const double with_csm = time_ms(f_hiz | f_csm);
    const double with_gtao = time_ms(f_hiz | f_gtao);
    const double lit = time_ms(f_lighting);
    const double lit_csm = time_ms(f_hiz | f_csm | f_lighting);
    const double lit_all = time_ms(f_hiz | f_csm | f_gtao | f_lighting | f_contact);
    const double lit_no_contact = time_ms(f_hiz | f_csm | f_gtao | f_lighting);
    std::printf("[r12 timing] %ux%u, median of %d runs (ms)\n", k_extent.width, k_extent.height,
                k_runs);
    std::printf("  gbuffer only              %8.2f\n", base);
    std::printf("  hi-z build                %8.2f\n", with_hiz - base);
    std::printf("  csm fit + 4 cascades      %8.2f\n", with_csm - with_hiz);
    std::printf("  gtao main + denoise       %8.2f\n", with_gtao - with_hiz);
    std::printf("  lighting (no occlusion)   %8.2f\n", lit - base);
    std::printf("  lighting csm sampling     %8.2f\n", (lit_csm - with_csm) - (lit - base));
    std::printf("  contact shadows           %8.2f\n", lit_all - lit_no_contact);
    std::printf("  full R12 frame (lit)      %8.2f\n", lit_all);
}
