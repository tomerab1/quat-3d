// The velocity target (R11, engine/renderer/velocity.hpp): every GBuffer
// writer stores the unjittered UV motion of its surface, which must equal the
// CPU reference for the surface point each pixel shows — for moving meshes,
// instances, skinned meshes, a moving camera over static geometry, the
// background, and wind-animated grass.

#include <array>
#include <cmath>
#include <cstdint>
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
#include "engine/renderer/velocity.hpp"
#include "engine/renderer/velocity_pass.hpp"
#include "engine/rhi/device.hpp"
#include "engine/rhi/render_graph.hpp"
#include "support/expect_ok.hpp"
#include "support/gpu_context.hpp"
#include "support/render_readback.hpp"

namespace {

namespace r = engine::renderer;
namespace rhi = engine::rhi;
namespace asset = engine::asset;
using engine::core::Error;
using quat::test::HostImage;

constexpr VkExtent2D k_extent{64, 64};

glm::mat4 perspective() {
    glm::mat4 p = glm::perspective(glm::radians(60.0F), 1.0F, 0.1F, 100.0F);
    p[1][1] *= -1.0F; // Vulkan y down
    return p;
}

glm::mat4 camera(const glm::vec3& eye) {
    return perspective() * glm::lookAt(eye, glm::vec3(0.0F), glm::vec3(0.0F, 1.0F, 0.0F));
}

glm::vec2 velocity_at(const HostImage& img, std::uint32_t x, std::uint32_t y) {
    std::uint16_t h[2];
    std::memcpy(h, img.at(x, y), sizeof(h));
    return {glm::unpackHalf1x16(h[0]), glm::unpackHalf1x16(h[1])};
}

// Unjittered NDC of the scene point pixel (x, y) shows (jitter.hpp).
glm::vec2 pixel_ndc(std::uint32_t x, std::uint32_t y, glm::vec2 jitter_ndc) {
    const glm::vec2 uv((static_cast<float>(x) + 0.5F) / static_cast<float>(k_extent.width),
                       (static_cast<float>(y) + 0.5F) / static_cast<float>(k_extent.height));
    return uv * 2.0F - 1.0F - jitter_ndc;
}

// Reference velocity of pixel (x, y) on a mesh lying in its object's z = 0
// plane, posed by `model` now and `prev_model` last frame. The pixel's ray is
// intersected with the plane in object space; that object point is then
// projected through both frames.
glm::vec2 plane_reference(std::uint32_t x, std::uint32_t y, const glm::mat4& model,
                          const glm::mat4& prev_model, const glm::mat4& vp,
                          const glm::mat4& prev_vp, glm::vec2 jitter_ndc = glm::vec2(0.0F)) {
    const glm::mat4 inv = glm::inverse(vp * model);
    const glm::vec2 ndc = pixel_ndc(x, y, jitter_ndc);
    glm::vec4 a = inv * glm::vec4(ndc, 0.0F, 1.0F);
    glm::vec4 b = inv * glm::vec4(ndc, 1.0F, 1.0F);
    a /= a.w;
    b /= b.w;
    const float t = a.z / (a.z - b.z);
    const glm::vec4 p(glm::vec3(a) + t * (glm::vec3(b) - glm::vec3(a)), 1.0F);
    return r::surface_velocity(glm::vec3(model * p), glm::vec3(prev_model * p), vp, prev_vp);
}

// Pixel containing the projection of `world`.
glm::uvec2 pixel_of(const glm::vec3& world, const glm::mat4& vp) {
    const glm::vec2 uv = r::project_to_uv(world, vp);
    return {static_cast<std::uint32_t>(uv.x * static_cast<float>(k_extent.width)),
            static_cast<std::uint32_t>(uv.y * static_cast<float>(k_extent.height))};
}

// A unit quad in the z = 0 plane, centred on the origin, facing +z.
std::expected<asset::MeshAsset, Error> quad(quat::test::GpuContext& g) {
    std::array<asset::Vertex, 4> v{};
    v[0].position = {-0.5F, -0.5F, 0.0F};
    v[1].position = {0.5F, -0.5F, 0.0F};
    v[2].position = {0.5F, 0.5F, 0.0F};
    v[3].position = {-0.5F, 0.5F, 0.0F};
    for (asset::Vertex& vert : v) vert.normal = {0.0F, 0.0F, 1.0F};
    const std::array<std::uint32_t, 6> indices{0, 1, 2, 0, 2, 3};
    return quat::test::upload_mesh(g, v, indices);
}

// Draws `list` into a GBuffer (rasterising with `raster_vp`), completes the
// background with VelocityPass (`vp` = unjittered view-projection) and returns
// the velocity target. `before` records into the command buffer first.
template <typename Before>
std::expected<HostImage, Error> render_velocity(quat::test::GpuContext& g,
                                                const r::InstancedDrawList& list,
                                                const glm::mat4& raster_vp, const glm::mat4& vp,
                                                const r::ViewMotion& motion, Before&& before) {
    auto pass = r::MeshPass::create(g.device, g.allocator, g.cache, g.transfer, g.shader_dir);
    if (!pass) return std::unexpected(pass.error());
    auto background = r::VelocityPass::create(g.device, g.cache, g.shader_dir);
    if (!background) return std::unexpected(background.error());
    rhi::TransientImagePool pool(g.device, g.allocator);
    rhi::RenderGraph graph(pool);
    auto targets = pass->add_to_graph(graph, k_extent, raster_vp, list, motion);
    if (!targets) return std::unexpected(targets.error());
    background->add_background_to_graph(graph, *targets, k_extent, vp);
    return quat::test::render_to_host(g, graph, targets->velocity, k_extent, 4,
                                      std::forward<Before>(before));
}

std::expected<HostImage, Error> render_velocity(quat::test::GpuContext& g,
                                                const r::InstancedDrawList& list,
                                                const glm::mat4& raster_vp, const glm::mat4& vp,
                                                const r::ViewMotion& motion) {
    return render_velocity(g, list, raster_vp, vp, motion, [](VkCommandBuffer) {});
}

// Half-float storage plus the rasteriser's interpolation: an absolute floor
// and a small relative part.
void expect_velocity(const glm::vec2& got, const glm::vec2& want, const char* what) {
    const float tol = 2e-4F + 2e-3F * glm::length(want);
    EXPECT_NEAR(got.x, want.x, tol) << what;
    EXPECT_NEAR(got.y, want.y, tol) << what;
}

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
