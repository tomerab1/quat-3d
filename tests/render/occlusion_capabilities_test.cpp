// Devices without shaderStorageImageExtendedFormats (R12): Hi-Z (RG32F) and
// GTAO (R8 edges) refuse to be created with a clear error, and the frame
// degrades as frame_graph.cpp does — no Hi-Z, so the cascades are fitted on
// the CPU, and no AO — while the sun shadows still render.

#include <cstring>
#include <string>
#include <vector>

#include <glm/glm.hpp>
#include <gtest/gtest.h>
#include <vulkan/vulkan.h>

#include "engine/renderer/csm.hpp"
#include "engine/renderer/gtao_pass.hpp"
#include "engine/renderer/hiz_pass.hpp"
#include "engine/renderer/lighting_pass.hpp"
#include "engine/renderer/mesh_pass.hpp"
#include "engine/renderer/shadow_pass.hpp"
#include "engine/rhi/device.hpp"
#include "engine/rhi/render_graph.hpp"
#include "render/occlusion_fixtures.hpp"
#include "support/expect_ok.hpp"
#include "support/gpu_context.hpp"
#include "support/render_readback.hpp"

namespace {

namespace r = engine::renderer;
namespace rhi = engine::rhi;
namespace occ = quat::test::occlusion;

constexpr VkExtent2D k_extent{96, 96};

// Restores the shared test device whatever the test does.
struct SimulatedMissingFormats {
    explicit SimulatedMissingFormats(rhi::Device& d) : device(d) {
        device.simulate_missing_extended_storage_formats(true);
    }
    ~SimulatedMissingFormats() { device.simulate_missing_extended_storage_formats(false); }
    SimulatedMissingFormats(const SimulatedMissingFormats&) = delete;
    SimulatedMissingFormats& operator=(const SimulatedMissingFormats&) = delete;
    rhi::Device& device;
};

} // namespace

TEST(GpuR12Capabilities, HiZAndGtaoRefuseDevicesWithoutExtendedStorageFormats) {
    QUAT_REQUIRE_GPU(g);
    ASSERT_TRUE(g->device.storage_image_extended_formats());
    const SimulatedMissingFormats missing(g->device);
    EXPECT_FALSE(g->device.storage_image_extended_formats());

    auto hiz = r::HiZPass::create(g->device, g->allocator, g->cache, g->shader_dir);
    ASSERT_ERR(hiz);
    EXPECT_NE(hiz.error().message.find("shaderStorageImageExtendedFormats"), std::string::npos)
        << hiz.error().message;
    EXPECT_NE(hiz.error().message.find("hi-z"), std::string::npos);
    auto gtao = r::GtaoPass::create(g->device, g->allocator, g->cache, g->shader_dir);
    ASSERT_ERR(gtao);
    EXPECT_NE(gtao.error().message.find("shaderStorageImageExtendedFormats"), std::string::npos)
        << gtao.error().message;
    EXPECT_FALSE(r::HiZPass{}.valid());
    EXPECT_FALSE(r::GtaoPass{}.valid());
}

TEST(GpuR12Capabilities, FrameWithoutHiZFitsCascadesOnTheCpuAndStillShadows) {
    QUAT_REQUIRE_GPU(g);
    const SimulatedMissingFormats missing(g->device);
    auto floor = occ::upload_box(*g, {-10.0F, -0.5F, -10.0F}, {10.0F, 0.0F, 10.0F});
    auto blocker = occ::upload_box(*g, {-0.5F, 1.5F, -0.5F}, {0.5F, 2.5F, 0.5F});
    auto grey = occ::upload_grey(*g);
    ASSERT_OK(floor);
    ASSERT_OK(blocker);
    ASSERT_OK(grey);
    // The passes the degraded frame still has.
    auto mesh = r::MeshPass::create(g->device, g->allocator, g->cache, g->transfer, g->shader_dir);
    auto shadow = r::ShadowPass::create(g->device, g->allocator, g->cache, g->shader_dir);
    auto lighting = r::LightingPass::create(g->device, g->allocator, g->cache, g->shader_dir);
    ASSERT_OK(mesh);
    ASSERT_OK(shadow);
    ASSERT_OK(lighting);

    const occ::Camera cam = occ::look_at({0.0F, 7.0F, 7.0F}, {0.0F, 0.0F, 0.0F});
    const glm::vec3 to_light(0.0F, 1.0F, 0.0F);
    const std::vector<r::DrawItem> draws{{&*floor, &*grey, glm::mat4(1.0F)},
                                         {&*blocker, &*grey, glm::mat4(1.0F)}};
    rhi::TransientImagePool pool(g->device, g->allocator);
    rhi::RenderGraph graph(pool);
    auto gbuffer = mesh->add_to_graph(graph, k_extent, cam.view_proj, draws);
    ASSERT_OK(gbuffer);
    // As frame_graph.cpp: no valid Hi-Z -> null depth bounds (GPU fit asked for).
    r::CsmSettings csm;
    ASSERT_TRUE(csm.gpu_depth_bounds);
    auto shadows = shadow->add_to_graph(graph, {cam.view, cam.proj, to_light}, csm, nullptr,
                                        draws);
    ASSERT_OK(shadows);
    ASSERT_TRUE(shadows->valid());
    r::DirectionalLightParams sun;
    sun.direction = glm::vec4(to_light, 0.0F);
    sun.color = {1.0F, 1.0F, 1.0F, 3.0F};
    r::LightingOcclusion occlusion; // no AO handle: GTAO is unavailable
    occlusion.shadows = *shadows;
    occlusion.csm = csm;
    occlusion.projection = cam.proj;
    auto hdr = lighting->add_to_graph(graph, *gbuffer, k_extent, sun,
                                      glm::inverse(cam.view_proj), cam.position, occlusion);
    ASSERT_OK(hdr);
    auto img = quat::test::render_to_host(*g, graph, *hdr, k_extent, 8);
    ASSERT_OK(img);

    r::GpuCascades fitted{};
    std::memcpy(&fitted, shadow->cascade_buffer().mapped(), sizeof(fitted));
    EXPECT_EQ(fitted.params.w, 0.0F); // the CPU reference fitted them
    const glm::uvec2 under = occ::project(cam.view_proj, {0.0F, 0.0F, 0.0F}, k_extent);
    const glm::uvec2 open = occ::project(cam.view_proj, {3.0F, 0.0F, 2.0F}, k_extent);
    const float dark = img->rgba16f(under.x, under.y).r;
    const float lit = img->rgba16f(open.x, open.y).r;
    EXPECT_GT(lit, 0.2F);
    EXPECT_LT(dark, 0.35F * lit);
}
