// The velocity target (R11, engine/renderer/velocity.hpp): every GBuffer
// writer stores the unjittered UV motion of its surface, which must equal the
// CPU reference for the surface point each pixel shows — for moving meshes,
// instances, skinned meshes, a moving camera over static geometry, the
// background, terrain under a moving camera, and wind-animated grass (both
// LOD regions).

#include <array>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <expected>
#include <span>
#include <vector>

#include <glm/glm.hpp>
#include <glm/gtc/matrix_transform.hpp>
#include <glm/gtc/packing.hpp>
#include <gtest/gtest.h>
#include <vulkan/vulkan.h>

#include "engine/asset/material_asset.hpp"
#include "engine/asset/mesh_asset.hpp"
#include "engine/renderer/draw_batching.hpp"
#include "engine/renderer/grass_pass.hpp"
#include "engine/renderer/jitter.hpp"
#include "engine/renderer/mesh_pass.hpp"
#include "engine/renderer/skinning_pass.hpp"
#include "engine/renderer/terrain_pass.hpp"
#include "engine/renderer/velocity.hpp"
#include "engine/renderer/velocity_pass.hpp"
#include "engine/rhi/device.hpp"
#include "engine/rhi/render_graph.hpp"
#include "engine/terrain/heightmap.hpp"
#include "support/expect_ok.hpp"
#include "support/gpu_context.hpp"
#include "render/velocity_rig.hpp"
#include "support/render_readback.hpp"

namespace {

namespace r = engine::renderer;
namespace rhi = engine::rhi;
namespace asset = engine::asset;
using engine::core::Error;
using quat::test::HostImage;

using namespace quat::test::velocity; // shared velocity-test helpers

} // namespace

// A quad translated between frames under a static camera: each covered
// pixel's velocity is its surface point's screen motion.
TEST(GpuVelocity, TranslatingQuadMatchesItsScreenMotion) {
    QUAT_REQUIRE_GPU(g);
    auto mesh = quad(*g);
    ASSERT_OK(mesh);
    const glm::mat4 vp = camera(glm::vec3(0.0F, 0.0F, 3.0F));
    const glm::mat4 model = glm::translate(glm::mat4(1.0F), glm::vec3(0.2F, 0.0F, 0.0F));
    const glm::mat4 prev = glm::translate(glm::mat4(1.0F), glm::vec3(0.1F, 0.05F, -0.2F));
    r::DrawItem item;
    item.mesh = &*mesh;
    item.model = model;
    item.prev_model = prev;
    auto vel = render_velocity(*g, r::batch_draws({&item, 1}), vp, vp, r::ViewMotion::still(vp));
    ASSERT_OK(vel);

    const glm::uvec2 c = pixel_of(glm::vec3(0.2F, 0.0F, 0.0F), vp);
    const glm::vec2 centre = velocity_at(*vel, c.x, c.y);
    EXPECT_GT(glm::length(centre), 0.01F) << "no motion stored";
    for (const glm::ivec2 o : {glm::ivec2(0), glm::ivec2(-4, 3), glm::ivec2(5, -5)}) {
        const glm::uvec2 p = glm::uvec2(glm::ivec2(c) + o);
        expect_velocity(velocity_at(*vel, p.x, p.y),
                        plane_reference(p.x, p.y, model, prev, vp, vp), "covered pixel");
    }
}

// The jitter contract: rasterising with a jittered projection must not show
// up as motion. A static quad stays at zero velocity although every sample
// moved by the jitter (here 0.4 / 0.3 pixels).
TEST(GpuVelocity, ProjectionJitterIsRemoved) {
    QUAT_REQUIRE_GPU(g);
    auto mesh = quad(*g);
    ASSERT_OK(mesh);
    const glm::mat4 vp = camera(glm::vec3(0.3F, 0.2F, 3.0F));
    const glm::vec2 jitter = glm::vec2(0.4F, -0.3F) * 2.0F / 64.0F;
    r::DrawItem item;
    item.mesh = &*mesh;
    r::ViewMotion motion{vp, jitter};
    auto vel = render_velocity(*g, r::batch_draws({&item, 1}), r::jitter_view_proj(vp, jitter),
                               vp, motion);
    ASSERT_OK(vel);
    const glm::uvec2 c = pixel_of(glm::vec3(0.0F), vp);
    for (const glm::ivec2 o : {glm::ivec2(0), glm::ivec2(-5, 2), glm::ivec2(4, 4)}) {
        const glm::vec2 v = velocity_at(*vel, c.x + o.x, c.y + o.y);
        EXPECT_LT(glm::length(v), 2e-4F) << "jitter leaked into velocity: " << v.x << ", " << v.y;
    }
}

// Static geometry under a moving camera gets exactly the camera's motion,
// and the background (no geometry) is filled with the far plane's motion.
TEST(GpuVelocity, StaticSceneWithMovingCameraIsCameraOnly) {
    QUAT_REQUIRE_GPU(g);
    auto mesh = quad(*g);
    ASSERT_OK(mesh);
    const glm::mat4 vp = camera(glm::vec3(0.0F, 0.0F, 3.0F));
    const glm::mat4 prev_vp = camera(glm::vec3(0.25F, -0.1F, 3.2F));
    r::DrawItem item; // no prev_model: camera-only fallback
    item.mesh = &*mesh;
    auto vel = render_velocity(*g, r::batch_draws({&item, 1}), vp, vp, r::ViewMotion{prev_vp, {}});
    ASSERT_OK(vel);
    const glm::uvec2 c = pixel_of(glm::vec3(0.0F), vp);
    for (const glm::ivec2 o : {glm::ivec2(0), glm::ivec2(6, -3), glm::ivec2(-6, 6)}) {
        const glm::uvec2 p = glm::uvec2(glm::ivec2(c) + o);
        expect_velocity(velocity_at(*vel, p.x, p.y),
                        plane_reference(p.x, p.y, glm::mat4(1.0F), glm::mat4(1.0F), vp, prev_vp),
                        "static surface");
    }
    // Background: the far-plane point behind pixel (2, 2), reprojected.
    const glm::vec2 ndc = pixel_ndc(2, 2, glm::vec2(0.0F));
    const glm::vec4 far_point = glm::inverse(vp) * glm::vec4(ndc, 1.0F, 1.0F);
    const glm::vec4 prev_clip = prev_vp * far_point;
    const glm::vec2 want = (ndc - glm::vec2(prev_clip) / prev_clip.w) * 0.5F;
    EXPECT_GT(glm::length(want), 1e-3F);
    expect_velocity(velocity_at(*vel, 2, 2), want, "background");
}

// Two instances of one mesh batched into a single instanced draw, each with
// its own previous pose: every instance carries its own motion.
TEST(GpuVelocity, InstancesCarryTheirOwnMotion) {
    QUAT_REQUIRE_GPU(g);
    auto mesh = quad(*g);
    ASSERT_OK(mesh);
    const glm::mat4 vp = camera(glm::vec3(0.0F, 0.0F, 3.0F));
    const glm::mat4 scale = glm::scale(glm::mat4(1.0F), glm::vec3(0.6F));
    std::array<r::DrawItem, 2> items{};
    items[0].mesh = items[1].mesh = &*mesh;
    items[0].model = glm::translate(glm::mat4(1.0F), glm::vec3(-0.6F, 0.0F, 0.0F)) * scale;
    items[0].prev_model = glm::translate(glm::mat4(1.0F), glm::vec3(-0.7F, 0.0F, 0.0F)) * scale;
    items[1].model = glm::translate(glm::mat4(1.0F), glm::vec3(0.6F, 0.1F, 0.0F)) * scale;
    items[1].prev_model = glm::translate(glm::mat4(1.0F), glm::vec3(0.6F, 0.25F, 0.0F)) * scale;
    const r::InstancedDrawList list = r::batch_draws(items);
    ASSERT_EQ(list.draws.size(), 1U);
    ASSERT_EQ(list.draws[0].instance_count, 2U);
    auto vel = render_velocity(*g, list, vp, vp, r::ViewMotion::still(vp));
    ASSERT_OK(vel);
    for (const r::DrawItem& item : items) {
        const glm::uvec2 p = pixel_of(glm::vec3(item.model[3]), vp);
        const glm::vec2 want = plane_reference(p.x, p.y, item.model, *item.prev_model, vp, vp);
        EXPECT_GT(glm::length(want), 0.01F);
        expect_velocity(velocity_at(*vel, p.x, p.y), want, "instance");
    }
}

// A skinned quad: the skinning pass writes last frame's skinned positions,
// and the GBuffer's velocity matches CPU skinning with both palettes.
TEST(GpuVelocity, SkinnedMeshMatchesCpuSkinning) {
    QUAT_REQUIRE_GPU(g);
    const VkDevice vk = g->device.handle();
    auto bind = quad(*g);
    ASSERT_OK(bind);
    std::array<asset::Vertex, 4> verts{};
    verts[0].position = {-0.5F, -0.5F, 0.0F};
    verts[1].position = {0.5F, -0.5F, 0.0F};
    verts[2].position = {0.5F, 0.5F, 0.0F};
    verts[3].position = {-0.5F, 0.5F, 0.0F};
    for (asset::Vertex& v : verts) v.normal = {0.0F, 0.0F, 1.0F};
    const asset::SkinVertex joint0{{0, 0, 0, 0}, {1, 0, 0, 0}};
    const std::array<asset::SkinVertex, 4> skin{joint0, joint0, joint0, joint0};
    const glm::mat4 joint = glm::rotate(glm::translate(glm::mat4(1.0F), glm::vec3(0.15F, 0.0F, 0.0F)),
                                        glm::radians(12.0F), glm::vec3(0.0F, 0.0F, 1.0F));
    const glm::mat4 prev_joint = glm::translate(glm::mat4(1.0F), glm::vec3(0.0F, -0.1F, 0.0F));

    const auto storage = [&](const void* data, VkDeviceSize size) {
        return rhi::upload_device_buffer(g->allocator, g->transfer, data, size,
                                         VK_BUFFER_USAGE_STORAGE_BUFFER_BIT |
                                             VK_BUFFER_USAGE_SHADER_DEVICE_ADDRESS_BIT);
    };
    auto vin = storage(verts.data(), sizeof(verts));
    auto sin = storage(skin.data(), sizeof(skin));
    auto jin = storage(&joint, sizeof(joint));
    auto pjin = storage(&prev_joint, sizeof(prev_joint));
    ASSERT_OK(vin);
    ASSERT_OK(sin);
    ASSERT_OK(jin);
    ASSERT_OK(pjin);
    auto vout = g->allocator.create_buffer(
        sizeof(verts),
        VK_BUFFER_USAGE_STORAGE_BUFFER_BIT | VK_BUFFER_USAGE_VERTEX_BUFFER_BIT |
            VK_BUFFER_USAGE_SHADER_DEVICE_ADDRESS_BIT,
        VMA_MEMORY_USAGE_AUTO, 0);
    auto prev_out = g->allocator.create_buffer(
        4 * sizeof(glm::vec4),
        VK_BUFFER_USAGE_STORAGE_BUFFER_BIT | VK_BUFFER_USAGE_SHADER_DEVICE_ADDRESS_BIT,
        VMA_MEMORY_USAGE_AUTO, 0);
    ASSERT_OK(vout);
    ASSERT_OK(prev_out);

    r::SkinDispatch d{};
    d.vertices_in = rhi::buffer_device_address(vk, *vin);
    d.vertices_in_size = sizeof(verts);
    d.skin = rhi::buffer_device_address(vk, *sin);
    d.skin_size = sizeof(skin);
    d.joints = rhi::buffer_device_address(vk, *jin);
    d.joints_size = sizeof(joint);
    d.prev_joints = rhi::buffer_device_address(vk, *pjin);
    d.vertices_out = rhi::buffer_device_address(vk, *vout);
    d.vertices_out_size = sizeof(verts);
    d.prev_positions_out = rhi::buffer_device_address(vk, *prev_out);
    d.prev_positions_out_size = 4 * sizeof(glm::vec4);
    d.vertex_count = 4;
    auto skinning = r::SkinningPass::create(g->device, g->allocator, g->cache, g->shader_dir);
    ASSERT_OK(skinning);

    // Skinned vertices are in skin space: identity model, both frames.
    r::DrawItem item;
    item.mesh = &*bind;
    item.skinned_vertices = vout->handle();
    item.prev_model = glm::mat4(1.0F);
    item.prev_positions = d.prev_positions_out;
    const glm::mat4 vp = camera(glm::vec3(0.0F, 0.0F, 3.0F));
    std::expected<void, Error> recorded;
    auto vel = render_velocity(*g, r::batch_draws({&item, 1}), vp, vp, r::ViewMotion::still(vp),
                               [&](VkCommandBuffer cmd) { recorded = skinning->record(cmd, {&d, 1}); });
    ASSERT_OK(recorded);
    ASSERT_OK(vel);
    const glm::uvec2 c = pixel_of(glm::vec3(joint[3]), vp);
    for (const glm::ivec2 o : {glm::ivec2(0), glm::ivec2(3, -4), glm::ivec2(-4, 2)}) {
        const glm::uvec2 p = glm::uvec2(glm::ivec2(c) + o);
        const glm::vec2 want = plane_reference(p.x, p.y, joint, prev_joint, vp, vp);
        EXPECT_GT(glm::length(want), 0.01F);
        expect_velocity(velocity_at(*vel, p.x, p.y), want, "skinned pixel");
    }
}

// Grass bends in the wind: blades evaluated at the previous wind time give
// their pixels motion under a static camera; with an unchanged wind clock the
// field is still (zero velocity).
TEST(GpuVelocity, GrassWindProducesMotion) {
    QUAT_REQUIRE_GPU(g);
    auto pass = r::GrassPass::create(g->device, g->allocator, g->cache, g->transfer, g->shader_dir,
                                     1);
    ASSERT_OK(pass);
    constexpr VkExtent2D extent{96, 96};
    r::GrassFrame frame;
    frame.settings.terrain_only = false;
    frame.settings.wind_strength = 1.0F;
    frame.camera = glm::vec3(0.0F, 1.2F, 0.0F);
    const glm::mat4 view =
        glm::lookAt(frame.camera, glm::vec3(10.0F, 0.0F, 0.0F), glm::vec3(0.0F, 1.0F, 0.0F));
    glm::mat4 proj = glm::perspective(glm::radians(70.0F), 1.0F, 0.1F, 200.0F);
    proj[1][1] *= -1.0F;
    frame.cull_view_proj = frame.draw_view_proj = proj * view;
    frame.time_s = 2.0F;

    const auto moving_pixels = [&](float prev_time) -> std::expected<std::size_t, Error> {
        auto mesh = r::MeshPass::create(g->device, g->allocator, g->cache, g->transfer,
                                        g->shader_dir);
        if (!mesh) return std::unexpected(mesh.error());
        frame.prev_time_s = prev_time;
        rhi::TransientImagePool pool(g->device, g->allocator);
        rhi::RenderGraph graph(pool);
        auto gbuffer = mesh->add_to_graph(graph, extent, frame.draw_view_proj,
                                          r::InstancedDrawList{});
        if (!gbuffer) return std::unexpected(gbuffer.error());
        auto grass = pass->add_to_graph(graph, 0, frame, &*gbuffer, extent);
        if (!grass) return std::unexpected(grass.error());
        auto vel = quat::test::render_to_host(*g, graph, gbuffer->velocity, extent, 4);
        if (!vel) return std::unexpected(vel.error());
        std::size_t moving = 0;
        for (std::uint32_t y = extent.height * 3 / 4; y < extent.height; ++y) {
            for (std::uint32_t x = 0; x < extent.width; ++x) {
                const glm::vec2 v = velocity_at(*vel, x, y);
                if (glm::length(v) * static_cast<float>(extent.width) > 0.05F) ++moving;
            }
        }
        return moving;
    };
    auto still = moving_pixels(frame.time_s);
    ASSERT_OK(still);
    EXPECT_EQ(*still, 0U) << "an unchanged wind clock must not produce motion";
    auto windy = moving_pixels(frame.time_s - 0.25F);
    ASSERT_OK(windy);
    EXPECT_GT(*windy, static_cast<std::size_t>(extent.width) * (extent.height / 4) / 4)
        << "wind-animated blades wrote no motion";
}

// Terrain is static: under a moving camera each ground pixel stores the camera
// motion of the point it shows.
TEST(GpuVelocity, TerrainUnderAMovingCameraIsCameraMotion) {
    QUAT_REQUIRE_GPU(g);
    auto mesh = r::MeshPass::create(g->device, g->allocator, g->cache, g->transfer, g->shader_dir);
    ASSERT_OK(mesh);
    auto terrain =
        r::TerrainPass::create(g->device, g->allocator, g->cache, g->transfer, g->shader_dir);
    ASSERT_OK(terrain);
    engine::terrain::Heightmap flat;
    flat.resolution = 65;
    flat.tile_size_m = 64.0F;
    flat.heights.assign(static_cast<std::size_t>(65) * 65, 0.0F);
    ASSERT_OK(terrain->set_tile(glm::ivec2(0, 0), flat, g->transfer));

    const glm::vec3 eye(0.0F, 10.0F, 12.0F);
    const glm::mat4 vp = camera(eye);
    const glm::mat4 prev_vp = camera(glm::vec3(1.0F, 10.5F, 12.0F));
    rhi::TransientImagePool pool(g->device, g->allocator);
    rhi::RenderGraph graph(pool);
    auto gbuffer = mesh->add_to_graph(graph, k_extent, vp, r::InstancedDrawList{},
                                      r::ViewMotion{prev_vp, glm::vec2(0.0F)});
    ASSERT_OK(gbuffer);
    terrain->add_to_graph(graph, *gbuffer, k_extent, vp, eye, glm::vec3(0.0F),
                          /*snowline_m=*/1000.0F);
    auto vel = quat::test::render_to_host(*g, graph, gbuffer->velocity, k_extent, 4);
    ASSERT_OK(vel);

    // The ground (world y = 0) as an object z = 0 plane: rotate -90 degrees about x.
    const glm::mat4 ground =
        glm::rotate(glm::mat4(1.0F), glm::radians(-90.0F), glm::vec3(1.0F, 0.0F, 0.0F));
    for (const glm::uvec2 p : {glm::uvec2(32, 32), glm::uvec2(10, 50), glm::uvec2(50, 40)}) {
        const glm::vec2 want = plane_reference(p.x, p.y, ground, ground, vp, prev_vp);
        EXPECT_GT(glm::length(want), 1e-3F);
        expect_velocity(velocity_at(*vel, p.x, p.y), want, "terrain pixel");
    }
}

// The far LOD list starts at the near region's capacity (push constant
// `base`; the indirect draws keep firstInstance = 0). Short blades keep the
// near disc (within 16 m) below the screen band showing ground 20-36 m away,
// so only far blades can write motion there; and the near region's capacity
// exceeds the far count, so a draw that ignored `base` would read only near
// slots (empty, never written by this fresh pass) and leave the band still.
TEST(GpuVelocity, FarLodGrassWritesMotionInItsOwnRegion) {
    QUAT_REQUIRE_GPU(g);
    auto pass = r::GrassPass::create(g->device, g->allocator, g->cache, g->transfer, g->shader_dir,
                                     1);
    ASSERT_OK(pass);
    auto mesh = r::MeshPass::create(g->device, g->allocator, g->cache, g->transfer, g->shader_dir);
    ASSERT_OK(mesh);
    constexpr VkExtent2D extent{128, 128};
    r::GrassFrame frame;
    frame.settings.terrain_only = false;
    frame.settings.radius = 40.0F;
    frame.settings.lod_start = 0.4F; // near disc: 16 m
    frame.settings.height_range = glm::vec2(0.1F, 0.15F);
    frame.settings.wind_strength = 1.0F;
    // A raised, narrow view spreads the 20-36 m ground over many rows.
    frame.camera = glm::vec3(0.0F, 3.0F, 0.0F);
    const glm::mat4 view =
        glm::lookAt(frame.camera, glm::vec3(12.0F, 0.0F, 0.0F), glm::vec3(0.0F, 1.0F, 0.0F));
    glm::mat4 proj = glm::perspective(glm::radians(30.0F), 1.0F, 0.1F, 200.0F);
    proj[1][1] *= -1.0F;
    frame.cull_view_proj = frame.draw_view_proj = proj * view;
    frame.time_s = 2.0F;
    frame.prev_time_s = 1.75F;

    rhi::TransientImagePool pool(g->device, g->allocator);
    rhi::RenderGraph graph(pool);
    auto gbuffer =
        mesh->add_to_graph(graph, extent, frame.draw_view_proj, r::InstancedDrawList{});
    ASSERT_OK(gbuffer);
    auto grass = pass->add_to_graph(graph, 0, frame, &*gbuffer, extent);
    ASSERT_OK(grass);
    ASSERT_TRUE(grass->valid());
    auto args_host = quat::test::host_readback_buffer(g->allocator, sizeof(r::GrassArgsGpu));
    ASSERT_OK(args_host);
    const VkBuffer args_dst = args_host->handle();
    const rhi::ResourceHandle args = grass->args;
    (void)graph.add_pass("args_readback", rhi::PassType::transfer)
        .reads(args, rhi::ResourceUsage::transfer_src)
        .execute([=](rhi::PassContext& ctx) {
            const VkBufferCopy copy{0, 0, sizeof(r::GrassArgsGpu)};
            vkCmdCopyBuffer(ctx.cmd(), ctx.resolve(args).buffer, args_dst, 1, &copy);
        });
    auto vel = quat::test::render_to_host(*g, graph, gbuffer->velocity, extent, 4);
    ASSERT_OK(vel);
    ASSERT_OK(args_host->invalidate_mapped());
    r::GrassArgsGpu counts{};
    std::memcpy(&counts, args_host->mapped(), sizeof(counts));
    ASSERT_GT(grass->capacity.near, 0U) << "the far region must start past 0";
    ASSERT_GT(counts.far.instanceCount, 1000U);
    ASSERT_LT(counts.far.instanceCount, grass->capacity.near)
        << "precondition: a base-less draw would read only the near region";

    // Rows showing the ground 20 m .. 36 m ahead (far LOD only).
    const glm::uvec2 top = pixel_of(glm::vec3(36.0F, 0.0F, 0.0F), frame.draw_view_proj, extent);
    const glm::uvec2 bottom = pixel_of(glm::vec3(20.0F, 0.0F, 0.0F), frame.draw_view_proj, extent);
    ASSERT_LT(top.y + 2, bottom.y);
    std::size_t moving = 0;
    std::size_t band = 0;
    for (std::uint32_t y = top.y; y < bottom.y; ++y) {
        for (std::uint32_t x = 0; x < extent.width; ++x) {
            ++band;
            const glm::vec2 v = velocity_at(*vel, x, y);
            if (glm::length(v) * static_cast<float>(extent.width) > 0.02F) ++moving;
        }
    }
    std::printf("[grass] far band: %zu of %zu pixels moving (far blades %u, near capacity %u)\n",
                moving, band, counts.far.instanceCount, grass->capacity.near);
    EXPECT_GT(moving, band / 10) << "far LOD blades wrote no motion in their band";
}
