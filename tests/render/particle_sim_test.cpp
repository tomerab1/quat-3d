// GPU particles (R10.3): emission, lifetime, the CPU reference, determinism,
// depth-buffer collision, the back-to-front sort and pausing — the compute
// passes read back and checked.

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <map>
#include <vector>

#include <glm/glm.hpp>
#include <glm/gtc/packing.hpp>
#include <gtest/gtest.h>

#include "engine/renderer/particle_pass.hpp"
#include "engine/renderer/particles.hpp"
#include "render/particle_harness.hpp"
#include "support/expect_ok.hpp"
#include "support/gpu_context.hpp"

namespace {

namespace r = engine::renderer;
namespace rhi = engine::rhi;
using quat::test::Camera;
using quat::test::PoolSnapshot;
using quat::test::TestEmitter;

constexpr std::uint32_t k_pool = 8192;
constexpr float         k_dt = 1.0F / 60.0F;

[[nodiscard]] std::expected<r::ParticlePass, engine::core::Error> make_pass(quat::test::GpuContext& g) {
    return r::ParticlePass::create(g.device, g.allocator, g.cache, g.transfer, g.shader_dir, 1,
                                   k_pool);
}

// Steps `emitters` `frames` times; returns the last snapshot.
[[nodiscard]] std::expected<PoolSnapshot, engine::core::Error>
run(quat::test::GpuContext& g, r::ParticlePass& pass, std::vector<TestEmitter*> emitters,
    const Camera& cam, int frames, float dt, const quat::test::TargetsFn& targets = {},
    VkExtent2D extent = {64, 64}) {
    std::expected<PoolSnapshot, engine::core::Error> snap =
        std::unexpected(engine::core::Error{"no frames"});
    for (int f = 0; f < frames; ++f) {
        std::vector<r::ParticleEmitterInput> inputs;
        for (TestEmitter* e : emitters) inputs.push_back(e->input(dt));
        snap = quat::test::run_particle_frame(
            g, pass, quat::test::make_frame(inputs, cam, dt, extent), targets);
        if (!snap) return snap;
    }
    return snap;
}

[[nodiscard]] TestEmitter fountain(std::uint32_t slot, std::uint32_t base) {
    TestEmitter e;
    e.slot = slot;
    e.base = base;
    e.settings.max_particles = 2048;
    e.settings.rate = 120.0F;
    e.settings.lifetime = {100.0F, 100.0F};
    e.settings.shape = r::ParticleShape::sphere;
    e.settings.radius = 0.5F;
    e.settings.speed = {1.0F, 3.0F};
    e.settings.drag = 0.5F;
    e.world = glm::translate(glm::mat4(1.0F), glm::vec3(0.0F, 1.0F, 0.0F));
    e.settings = r::sanitize(e.settings);
    return e;
}

const Camera k_cam = quat::test::look_at({0.0F, 3.0F, 6.0F}, {0.0F, 0.0F, 0.0F});

TEST(GpuParticles, EmissionFollowsTheRate) {
    QUAT_REQUIRE_GPU(g);
    auto pass = make_pass(*g);
    ASSERT_OK(pass);
    TestEmitter a = fountain(3, 0);
    TestEmitter b = fountain(7, 2048);
    b.settings.rate = 45.0F;
    auto snap = run(*g, *pass, {&a, &b}, k_cam, 60, k_dt);
    ASSERT_OK(snap);
    // One second: rate x 1 s particles, all alive (lifetime 100 s).
    EXPECT_EQ(a.spawn.spawned, 120U);
    EXPECT_EQ(b.spawn.spawned, 45U);
    EXPECT_EQ(snap->drawn_count(a.slot), 120U);
    EXPECT_EQ(snap->drawn_count(b.slot), 45U);
    EXPECT_EQ(snap->dead(a.slot), 2048U - 120U);
    EXPECT_EQ(snap->dead(b.slot), 2048U - 45U);
    // Every drawn slot lies in its emitter's range and is distinct.
    std::vector<std::uint32_t> slots = snap->drawn_slots(b);
    for (const std::uint32_t s : slots) {
        EXPECT_GE(s, b.base);
        EXPECT_LT(s, b.base + b.settings.max_particles);
    }
    std::sort(slots.begin(), slots.end());
    EXPECT_EQ(std::adjacent_find(slots.begin(), slots.end()), slots.end());
}

TEST(GpuParticles, LifetimeKillsAndRecyclesSlots) {
    QUAT_REQUIRE_GPU(g);
    auto pass = make_pass(*g);
    ASSERT_OK(pass);
    TestEmitter e = fountain(0, 100);
    e.settings.rate = 0.0F;
    e.settings.burst_count = 300;
    e.settings.max_particles = 512;
    e.settings.lifetime = {0.45F, 0.45F};
    auto alive = run(*g, *pass, {&e}, k_cam, 4, 0.1F); // ages 0.1 .. 0.4
    ASSERT_OK(alive);
    EXPECT_EQ(alive->drawn_count(e.slot), 300U);
    EXPECT_EQ(alive->dead(e.slot), 512U - 300U);
    auto dead = run(*g, *pass, {&e}, k_cam, 1, 0.1F); // age 0.5 >= 0.45
    ASSERT_OK(dead);
    EXPECT_EQ(dead->drawn_count(e.slot), 0U);
    EXPECT_EQ(dead->dead(e.slot), 512U);
}

TEST(GpuParticles, PausedFramesFreezeTheSimulation) {
    QUAT_REQUIRE_GPU(g);
    auto pass = make_pass(*g);
    ASSERT_OK(pass);
    TestEmitter e = fountain(1, 0);
    auto before = run(*g, *pass, {&e}, k_cam, 10, k_dt);
    ASSERT_OK(before);
    auto paused = run(*g, *pass, {&e}, k_cam, 3, 0.0F);
    ASSERT_OK(paused);
    ASSERT_EQ(paused->drawn_count(e.slot), before->drawn_count(e.slot));
    const auto a = before->drawn_particles(e);
    const auto b = paused->drawn_particles(e);
    ASSERT_EQ(a.size(), b.size());
    EXPECT_EQ(std::memcmp(a.data(), b.data(), a.size() * sizeof(r::ParticleGpu)), 0);
}

// The particles keyed by their random bits (unique per spawn index here).
[[nodiscard]] std::map<std::uint32_t, r::ParticleGpu> by_random(const std::vector<r::ParticleGpu>& ps) {
    std::map<std::uint32_t, r::ParticleGpu> out;
    for (const r::ParticleGpu& p : ps) out.emplace(p.random, p);
    return out;
}

TEST(GpuParticles, MatchesTheCpuReferenceAndIsDeterministic) {
    QUAT_REQUIRE_GPU(g);
    TestEmitter cpu = fountain(5, 0);
    cpu.settings.lifetime = {0.2F, 0.6F}; // some die within the run
    cpu.settings.shape = r::ParticleShape::cone;
    cpu.settings.cone_angle_deg = 30.0F;
    cpu.settings.direction = glm::normalize(glm::vec3(0.3F, 1.0F, 0.1F));
    cpu.world = glm::rotate(cpu.world, 0.4F, glm::vec3(0.0F, 0.0F, 1.0F));
    cpu.settings = r::sanitize(cpu.settings);

    std::vector<std::map<std::uint32_t, r::ParticleGpu>> runs;
    for (int k = 0; k < 2; ++k) {
        auto pass = make_pass(*g);
        ASSERT_OK(pass);
        TestEmitter e = cpu;
        auto snap = run(*g, *pass, {&e}, k_cam, 45, k_dt);
        ASSERT_OK(snap);
        runs.push_back(by_random(snap->drawn_particles(e)));
        EXPECT_EQ(runs.back().size(), snap->drawn_count(e.slot)); // random bits unique
    }
    // Determinism: the same particles, bit for bit, whatever slots they took.
    ASSERT_EQ(runs[0].size(), runs[1].size());
    for (const auto& [key, p] : runs[0]) {
        const auto it = runs[1].find(key);
        ASSERT_NE(it, runs[1].end());
        EXPECT_EQ(std::memcmp(&p, &it->second, sizeof(p)), 0);
    }

    // CPU reference: the same schedule, spawn and integration.
    std::vector<r::ParticleGpu> live;
    for (int f = 0; f < 45; ++f) {
        r::EmitterFrameInfo info;
        info.world = cpu.world;
        info.slot = cpu.slot;
        info.spawn_index = cpu.spawn.spawned;
        info.spawn_count = r::advance_spawn(cpu.spawn, cpu.settings, k_dt);
        info.dt = k_dt;
        const r::EmitterGpu e = r::make_emitter_gpu(cpu.settings, info);
        for (std::uint32_t i = 0; i < info.spawn_count; ++i) {
            live.push_back(r::particle_spawn(e, info.spawn_index + i));
        }
        std::erase_if(live, [&](r::ParticleGpu& p) { return !r::particle_integrate(p, e, k_dt); });
    }
    const auto reference = by_random(live);
    ASSERT_EQ(reference.size(), runs[0].size());
    for (const auto& [key, want] : reference) {
        const auto it = runs[0].find(key);
        ASSERT_NE(it, runs[0].end());
        const r::ParticleGpu& got = it->second;
        EXPECT_NEAR(glm::distance(got.position, want.position), 0.0F, 1e-3F);
        EXPECT_NEAR(glm::distance(got.velocity, want.velocity), 0.0F, 1e-3F);
        EXPECT_NEAR(got.age, want.age, 1e-5F);
        EXPECT_NEAR(got.lifetime, want.lifetime, 1e-5F);
        EXPECT_NEAR(got.size, want.size, 1e-5F);
        EXPECT_NEAR(got.rotation, want.rotation, 1e-4F);
    }
}

// A ground plane (y = 0) as the GBuffer would hold it for `cam`: depth and
// octahedral normal (0, 1, 0) per pixel, sky (depth 1) where the ray misses.
struct PlaneImages {
    rhi::GpuImage  depth;
    rhi::ImageView depth_view;
    rhi::GpuImage  normal;
    rhi::ImageView normal_view;
};

[[nodiscard]] std::expected<PlaneImages, engine::core::Error>
ground_plane(quat::test::GpuContext& g, const Camera& cam, VkExtent2D extent) {
    const glm::mat4 vp = cam.proj * cam.view;
    const glm::mat4 inv = glm::inverse(vp);
    std::vector<float> depth(std::size_t{extent.width} * extent.height, 1.0F);
    std::vector<std::uint16_t> normal(depth.size() * 2, 0);
    for (std::uint32_t y = 0; y < extent.height; ++y) {
        for (std::uint32_t x = 0; x < extent.width; ++x) {
            const glm::vec2 ndc((static_cast<float>(x) + 0.5F) / static_cast<float>(extent.width) * 2.0F - 1.0F,
                                (static_cast<float>(y) + 0.5F) / static_cast<float>(extent.height) * 2.0F - 1.0F);
            const glm::vec4 a = inv * glm::vec4(ndc, 0.0F, 1.0F);
            const glm::vec4 b = inv * glm::vec4(ndc, 1.0F, 1.0F);
            const glm::vec3 p0 = glm::vec3(a) / a.w;
            const glm::vec3 dir = glm::vec3(b) / b.w - p0;
            const std::size_t i = std::size_t{y} * extent.width + x;
            normal[i * 2 + 1] = glm::packHalf1x16(1.0F); // oct(0, 1, 0) = (0, 1)
            if (dir.y >= 0.0F) continue;
            const float t = -p0.y / dir.y;
            const glm::vec4 clip = vp * glm::vec4(p0 + dir * t, 1.0F);
            depth[i] = clip.z / clip.w;
        }
    }
    PlaneImages out;
    auto d = rhi::upload_device_image(g.allocator, g.transfer, depth.data(), depth.size() * 4,
                                      extent, VK_FORMAT_R32_SFLOAT);
    if (!d) return std::unexpected(d.error());
    out.depth = std::move(*d);
    auto dv = rhi::create_image_view(g.device.handle(), out.depth.handle(), VK_FORMAT_R32_SFLOAT);
    if (!dv) return std::unexpected(dv.error());
    out.depth_view = std::move(*dv);
    auto n = rhi::upload_device_image(g.allocator, g.transfer, normal.data(), normal.size() * 2,
                                      extent, VK_FORMAT_R16G16_SFLOAT);
    if (!n) return std::unexpected(n.error());
    out.normal = std::move(*n);
    auto nv = rhi::create_image_view(g.device.handle(), out.normal.handle(), VK_FORMAT_R16G16_SFLOAT);
    if (!nv) return std::unexpected(nv.error());
    out.normal_view = std::move(*nv);
    return out;
}

TEST(GpuParticles, DepthCollisionKeepsParticlesAboveThePlane) {
    QUAT_REQUIRE_GPU(g);
    constexpr VkExtent2D extent{128, 128};
    auto plane = ground_plane(*g, k_cam, extent);
    ASSERT_OK(plane);
    const auto targets = [&](rhi::RenderGraph& graph) {
        constexpr VkImageLayout ro = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
        r::ParticleTargets t;
        t.depth = graph.import_image("plane_depth", plane->depth.handle(), plane->depth_view.handle(),
                                     VK_FORMAT_R32_SFLOAT, extent, ro, ro);
        t.normal = graph.import_image("plane_normal", plane->normal.handle(),
                                      plane->normal_view.handle(), VK_FORMAT_R16G16_SFLOAT, extent,
                                      ro, ro);
        return t;
    };
    float lowest[2] = {0.0F, 0.0F};
    for (int collide = 0; collide < 2; ++collide) {
        auto pass = make_pass(*g);
        ASSERT_OK(pass);
        TestEmitter e;
        e.settings.max_particles = 1024;
        e.settings.rate = 0.0F;
        e.settings.burst_count = 400;
        e.settings.lifetime = {100.0F, 100.0F};
        e.settings.shape = r::ParticleShape::box;
        e.settings.box_half_extents = glm::vec3(0.6F, 0.2F, 0.6F);
        e.settings.direction = glm::vec3(0.0F, -1.0F, 0.0F);
        e.settings.speed = {0.0F, 2.0F};
        e.settings.collide = collide != 0;
        e.settings.bounce = 0.3F;
        e.world = glm::translate(glm::mat4(1.0F), glm::vec3(0.0F, 1.0F, 0.0F));
        e.settings = r::sanitize(e.settings);
        auto snap = run(*g, *pass, {&e}, k_cam, 90, k_dt, targets, extent);
        ASSERT_OK(snap);
        const auto ps = snap->drawn_particles(e);
        ASSERT_EQ(ps.size(), 400U);
        lowest[collide] = 1e30F;
        for (const r::ParticleGpu& p : ps) lowest[collide] = std::min(lowest[collide], p.position.y);
    }
    EXPECT_LT(lowest[0], -1.0F) << "without collision the particles fall through";
    EXPECT_GT(lowest[1], -0.05F) << "collision keeps them on the plane";
}

TEST(GpuParticles, SortOrdersTheDrawnListBackToFront) {
    QUAT_REQUIRE_GPU(g);
    auto pass = make_pass(*g);
    ASSERT_OK(pass);
    TestEmitter e;
    e.slot = 9;
    e.base = 512;
    e.settings.max_particles = 2048; // 2048 keys: global + shared-memory merge stages
    e.settings.rate = 0.0F;
    e.settings.burst_count = 1500;
    e.settings.lifetime = {100.0F, 100.0F};
    e.settings.shape = r::ParticleShape::sphere;
    e.settings.radius = 3.0F;
    e.settings.speed = {0.0F, 0.0F};
    e.settings.gravity = glm::vec3(0.0F);
    e.settings.sort = true;
    e.settings = r::sanitize(e.settings);
    auto snap = run(*g, *pass, {&e}, k_cam, 2, k_dt);
    ASSERT_OK(snap);
    const auto ps = snap->drawn_particles(e);
    ASSERT_EQ(ps.size(), 1500U);
    std::vector<std::uint32_t> slots = snap->drawn_slots(e);
    std::sort(slots.begin(), slots.end());
    EXPECT_EQ(std::adjacent_find(slots.begin(), slots.end()), slots.end());
    for (std::size_t i = 1; i < ps.size(); ++i) {
        const float prev = -(k_cam.view * glm::vec4(ps[i - 1].position, 1.0F)).z;
        const float cur = -(k_cam.view * glm::vec4(ps[i].position, 1.0F)).z;
        ASSERT_GE(prev + 1e-4F, cur) << "at " << i;
    }
}

} // namespace
