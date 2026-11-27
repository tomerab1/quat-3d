// GPU particles (R10.3): the draw passes — weighted blended OIT is
// order-independent, soft particles fade near geometry, flipbooks and lit
// emitters render.

#include <cmath>
#include <cstdint>
#include <vector>

#include <glm/glm.hpp>
#include <glm/gtc/matrix_transform.hpp>
#include <gtest/gtest.h>

#include "engine/renderer/lighting_pass.hpp"
#include "engine/renderer/mesh_pass.hpp"
#include "engine/renderer/particle_pass.hpp"
#include "engine/renderer/particles.hpp"
#include "render/particle_harness.hpp"
#include "support/expect_ok.hpp"
#include "support/gpu_context.hpp"
#include "support/render_readback.hpp"

namespace {

namespace r = engine::renderer;
namespace rhi = engine::rhi;
using quat::test::Camera;
using quat::test::TestEmitter;

constexpr VkExtent2D k_extent{64, 64};
constexpr float      k_dt = 1.0F / 60.0F;

// The camera at the origin looking down -Z.
const Camera k_cam = quat::test::look_at({0.0F, 0.0F, 0.0F}, {0.0F, 0.0F, -1.0F});

// NDC depth of a fronto-parallel wall `distance` metres in front of k_cam.
[[nodiscard]] float wall_depth(float distance) {
    const glm::vec4 clip = k_cam.proj * glm::vec4(0.0F, 0.0F, -distance, 1.0F);
    return clip.z / clip.w;
}

// One still particle of `size` at (0, 0, -distance), constant colour.
[[nodiscard]] TestEmitter still_particle(std::uint32_t slot, std::uint32_t base, float distance,
                                         const glm::vec4& color, r::ParticleBlend blend) {
    TestEmitter e;
    e.slot = slot;
    e.base = base;
    e.settings.max_particles = 4;
    e.settings.rate = 0.0F;
    e.settings.burst_count = 1;
    e.settings.lifetime = {100.0F, 100.0F};
    e.settings.speed = {0.0F, 0.0F};
    e.settings.gravity = glm::vec3(0.0F);
    e.settings.size = {1.5F, 1.5F};
    e.settings.rotation_deg = {0.0F, 0.0F};
    e.settings.spin_deg = {0.0F, 0.0F};
    e.settings.color_keys = 1;
    e.settings.color_curve[0] = {0.0F, color};
    e.settings.blend = blend;
    e.world = glm::translate(glm::mat4(1.0F), glm::vec3(0.0F, 0.0F, -distance));
    e.settings = r::sanitize(e.settings);
    return e;
}

// Renders one frame of `inputs` over a black HDR with the depth cleared to
// `depth`; returns the HDR.
[[nodiscard]] std::expected<quat::test::HostImage, engine::core::Error>
render(quat::test::GpuContext& g, r::ParticlePass& pass,
       std::span<const r::ParticleEmitterInput> inputs, float dt, float depth) {
    rhi::TransientImagePool images(g.device, g.allocator);
    rhi::RenderGraph graph(images);
    const rhi::ResourceHandle hdr = graph.create_transient_image(
        "hdr", r::hdr_color_format, k_extent,
        VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT |
            VK_IMAGE_USAGE_TRANSFER_DST_BIT);
    const rhi::ResourceHandle depth_image = graph.create_transient_image(
        "depth", r::gbuffer_depth_format, k_extent,
        VK_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT | VK_IMAGE_USAGE_SAMPLED_BIT |
            VK_IMAGE_USAGE_TRANSFER_DST_BIT);
    (void)graph.add_pass("clear", rhi::PassType::transfer)
        .writes(hdr, rhi::ResourceUsage::transfer_dst)
        .writes(depth_image, rhi::ResourceUsage::transfer_dst)
        .execute([=](rhi::PassContext& ctx) {
            const VkClearColorValue black{{0.0F, 0.0F, 0.0F, 1.0F}};
            const VkImageSubresourceRange color{VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
            vkCmdClearColorImage(ctx.cmd(), ctx.resolve(hdr).image,
                                 VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, &black, 1, &color);
            const VkClearDepthStencilValue d{depth, 0};
            const VkImageSubresourceRange range{VK_IMAGE_ASPECT_DEPTH_BIT, 0, 1, 0, 1};
            vkCmdClearDepthStencilImage(ctx.cmd(), ctx.resolve(depth_image).image,
                                        VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, &d, 1, &range);
        });
    r::ParticleTargets targets;
    targets.depth = depth_image;
    targets.hdr = hdr;
    auto out = pass.add_to_graph(graph, 0, quat::test::make_frame(inputs, k_cam, dt, k_extent),
                                 targets);
    if (!out) return std::unexpected(out.error());
    return quat::test::render_to_host(g, graph, hdr, k_extent, 8);
}

[[nodiscard]] std::expected<r::ParticlePass, engine::core::Error> make_pass(quat::test::GpuContext& g) {
    return r::ParticlePass::create(g.device, g.allocator, g.cache, g.transfer, g.shader_dir, 1, 256);
}

TEST(GpuParticleDraw, WeightedBlendedOitIsOrderIndependent) {
    QUAT_REQUIRE_GPU(g);
    auto pass = make_pass(*g);
    ASSERT_OK(pass);
    TestEmitter red = still_particle(0, 0, 4.0F, {1.0F, 0.1F, 0.0F, 0.6F}, r::ParticleBlend::alpha);
    TestEmitter blue = still_particle(1, 4, 6.0F, {0.0F, 0.2F, 1.0F, 0.6F}, r::ParticleBlend::alpha);
    TestEmitter green = still_particle(2, 8, 5.0F, {0.1F, 1.0F, 0.1F, 0.5F}, r::ParticleBlend::alpha);
    const std::vector<r::ParticleEmitterInput> spawn{red.input(k_dt), blue.input(k_dt),
                                                     green.input(k_dt)};
    auto first = render(*g, *pass, spawn, k_dt, 1.0F);
    ASSERT_OK(first);
    // Same particles (paused: no simulation), drawn in the opposite order.
    const std::vector<r::ParticleEmitterInput> reversed{green.input(0.0F), blue.input(0.0F),
                                                        red.input(0.0F)};
    auto second = render(*g, *pass, reversed, 0.0F, 1.0F);
    ASSERT_OK(second);
    float max_diff = 0.0F;
    for (std::uint32_t y = 0; y < k_extent.height; ++y) {
        for (std::uint32_t x = 0; x < k_extent.width; ++x) {
            const glm::vec4 d = glm::abs(first->rgba16f(x, y) - second->rgba16f(x, y));
            max_diff = std::max({max_diff, d.r, d.g, d.b});
        }
    }
    EXPECT_LT(max_diff, 2e-3F);
    // All three contribute at the centre; the nearest (red) weighs most.
    const glm::vec4 c = first->rgba16f(32, 32);
    EXPECT_GT(c.r, c.g);
    EXPECT_GT(c.r, c.b);
    EXPECT_GT(c.b, 0.02F);
    EXPECT_GT(c.g, 0.1F);
    // Coverage 1 - (0.4 * 0.4 * 0.5) = 0.92 of the average over black.
    EXPECT_NEAR(first->rgba16f(1, 1).r, 0.0F, 1e-4F); // outside every quad
}

TEST(GpuParticleDraw, SoftParticlesFadeNearGeometry) {
    QUAT_REQUIRE_GPU(g);
    float centre[2] = {0.0F, 0.0F};
    const float walls[2] = {5.15F, 50.0F}; // 0.15 m and 45 m behind the particle
    for (int k = 0; k < 2; ++k) {
        auto pass = make_pass(*g);
        ASSERT_OK(pass);
        TestEmitter e = still_particle(0, 0, 5.0F, glm::vec4(1.0F), r::ParticleBlend::additive);
        e.settings.soft_distance = 0.3F;
        const std::vector<r::ParticleEmitterInput> in{e.input(k_dt)};
        auto img = render(*g, *pass, in, k_dt, wall_depth(walls[k]));
        ASSERT_OK(img);
        centre[k] = img->rgba16f(32, 32).r;
    }
    EXPECT_NEAR(centre[1], 1.0F, 0.02F);            // far from geometry: full
    EXPECT_NEAR(centre[0] / centre[1], 0.5F, 0.05F); // 0.15 / 0.3 of the way in
}

TEST(GpuParticleDraw, ParticlesBehindGeometryAreHidden) {
    QUAT_REQUIRE_GPU(g);
    auto pass = make_pass(*g);
    ASSERT_OK(pass);
    TestEmitter e = still_particle(0, 0, 8.0F, glm::vec4(1.0F), r::ParticleBlend::additive);
    e.settings.soft_distance = 0.0F;
    const std::vector<r::ParticleEmitterInput> in{e.input(k_dt)};
    auto img = render(*g, *pass, in, k_dt, wall_depth(4.0F)); // wall in front
    ASSERT_OK(img);
    EXPECT_NEAR(img->rgba16f(32, 32).r, 0.0F, 1e-4F);
}

TEST(GpuParticleDraw, LitParticlesTakeTheSceneLight) {
    QUAT_REQUIRE_GPU(g);
    float centre[2] = {0.0F, 0.0F};
    for (int lit = 0; lit < 2; ++lit) {
        auto pass = make_pass(*g);
        ASSERT_OK(pass);
        TestEmitter e = still_particle(0, 0, 5.0F, glm::vec4(0.5F, 0.5F, 0.5F, 1.0F),
                                       r::ParticleBlend::additive);
        e.settings.lighting = lit != 0 ? r::ParticleLighting::lit : r::ParticleLighting::unlit;
        e.settings.soft_distance = 0.0F;
        r::ParticleEmitterInput in = e.input(k_dt);
        // Unlit: colour x emissive (1). Lit: albedo x (ambient + sun / pi).
        auto img = render(*g, *pass, {&in, 1}, k_dt, 1.0F);
        ASSERT_OK(img);
        centre[lit] = img->rgba16f(32, 32).r;
    }
    EXPECT_NEAR(centre[0], 0.5F, 0.01F);
    // Default light: ambient 0.03, white sun of intensity 1 -> 0.5 * (0.03 + 1/pi).
    EXPECT_NEAR(centre[1], 0.5F * (0.03F + 1.0F / 3.14159265F), 0.01F);
}

// A 2-frame flipbook (red | green) played once over the lifetime: early in
// life the particle shows frame 0, late in life frame 1.
TEST(GpuParticleDraw, FlipbookAdvancesOverTheLifetime) {
    QUAT_REQUIRE_GPU(g);
    const std::uint8_t texels[8] = {255, 0, 0, 255, 0, 255, 0, 255};
    auto atlas = rhi::upload_device_image(g->allocator, g->transfer, texels, sizeof(texels), {2, 1},
                                          VK_FORMAT_R8G8B8A8_SRGB);
    ASSERT_OK(atlas);
    auto view = rhi::create_image_view(g->device.handle(), atlas->handle(), VK_FORMAT_R8G8B8A8_SRGB);
    ASSERT_OK(view);
    auto pass = make_pass(*g);
    ASSERT_OK(pass);
    TestEmitter e = still_particle(0, 0, 5.0F, glm::vec4(1.0F), r::ParticleBlend::additive);
    e.settings.lifetime = {1.0F, 1.0F};
    e.settings.flipbook_grid = {2, 1};
    e.settings.flipbook_frames = 2;
    e.settings.soft_distance = 0.0F;
    e.settings = r::sanitize(e.settings);
    glm::vec4 colour[2]{};
    for (int k = 0; k < 2; ++k) {
        // First image after one step (age 1/60), second after 45 (age 0.75).
        const int steps = k == 0 ? 1 : 44;
        for (int f = 0; f < steps; ++f) {
            r::ParticleEmitterInput in = e.input(k_dt);
            in.texture = view->handle();
            auto img = render(*g, *pass, {&in, 1}, k_dt, 1.0F);
            ASSERT_OK(img);
            colour[k] = img->rgba16f(32, 32);
        }
    }
    EXPECT_GT(colour[0].r, 0.9F);
    EXPECT_LT(colour[0].g, 0.1F);
    EXPECT_LT(colour[1].r, 0.1F);
    EXPECT_GT(colour[1].g, 0.9F);
}

} // namespace
