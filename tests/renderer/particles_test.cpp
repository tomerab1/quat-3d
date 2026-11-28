// GPU particles, CPU side (R10.3): emission schedule, pool allocation,
// settings sanitising, the GPU record and the spawn / integration references.

#include <cmath>
#include <cstdint>
#include <set>

#include <glm/glm.hpp>
#include <glm/gtc/matrix_transform.hpp>
#include <gtest/gtest.h>

#include "engine/renderer/particles.hpp"

namespace {

namespace r = engine::renderer;

TEST(ParticleSpawn, RateAccumulatesFractionalParticles) {
    r::ParticleEmitterSettings s;
    s.rate = 50.0F;
    s.max_particles = 4096;
    r::ParticleSpawnState state;
    std::uint32_t total = 0;
    for (int f = 0; f < 600; ++f) total += r::advance_spawn(state, s, 1.0F / 60.0F);
    EXPECT_NEAR(static_cast<double>(total), 500.0, 1.0); // 10 s at 50 / s
    EXPECT_EQ(state.spawned, total);
    // Irregular steps add up the same.
    r::ParticleSpawnState jitter;
    std::uint32_t uneven = 0;
    for (int f = 0; f < 400; ++f) uneven += r::advance_spawn(jitter, s, f % 2 == 0 ? 0.01F : 0.04F);
    EXPECT_NEAR(static_cast<double>(uneven), 500.0, 1.0);
}

TEST(ParticleSpawn, BurstsFireOnSchedule) {
    r::ParticleEmitterSettings s;
    s.rate = 0.0F;
    s.burst_count = 10;
    s.burst_interval = 0.5F;
    r::ParticleSpawnState state;
    EXPECT_EQ(r::advance_spawn(state, s, 0.1F), 10U); // t = 0
    std::uint32_t later = 0;
    for (int f = 0; f < 7; ++f) later += r::advance_spawn(state, s, 0.1F); // up to t = 0.8
    EXPECT_EQ(later, 10U);                                                  // t = 0.5

    s.burst_interval = 0.0F; // a single burst
    r::ParticleSpawnState once;
    std::uint32_t total = 0;
    for (int f = 0; f < 20; ++f) total += r::advance_spawn(once, s, 0.1F);
    EXPECT_EQ(total, 10U);
}

TEST(ParticleSpawn, PausedDisabledAndCappedSteps) {
    r::ParticleEmitterSettings s;
    s.rate = 1000.0F;
    s.max_particles = 16;
    r::ParticleSpawnState state;
    EXPECT_EQ(r::advance_spawn(state, s, 0.0F), 0U);
    EXPECT_EQ(r::advance_spawn(state, s, std::nanf("")), 0U);
    EXPECT_EQ(r::advance_spawn(state, s, 1.0F), 16U); // capped at the emitter's pool share
    s.enabled = false;
    EXPECT_EQ(r::advance_spawn(state, s, 1.0F), 0U);
}

TEST(ParticlePool, FirstFitAllocationMergesFreedRanges) {
    r::ParticlePoolAllocator pool(1000);
    const std::uint32_t a = pool.allocate(300);
    const std::uint32_t b = pool.allocate(300);
    const std::uint32_t c = pool.allocate(300);
    EXPECT_EQ(a, 0U);
    EXPECT_EQ(b, 300U);
    EXPECT_EQ(c, 600U);
    EXPECT_EQ(pool.allocate(200), UINT32_MAX); // 100 left
    pool.free(b, 300);
    EXPECT_EQ(pool.allocate(250), 300U);       // first fit reuses the hole
    pool.free(300, 250);
    pool.free(a, 300);
    pool.free(c, 300);
    EXPECT_EQ(pool.free_total(), 1000U);
    EXPECT_EQ(pool.allocate(1000), 0U);         // everything merged back
    pool.free(5000, 10);                        // out of range: ignored
    EXPECT_EQ(pool.free_total(), 0U);
}

TEST(ParticleSettings, SanitizeClampsOrdersAndNormalises) {
    r::ParticleEmitterSettings s;
    s.max_particles = 0;
    s.lifetime = {3.0F, -1.0F};
    s.direction = glm::vec3(0.0F);
    s.flipbook_grid = {4, 0};
    s.flipbook_frames = 99;
    s.color_keys = 3;
    s.color_curve[0] = {0.9F, glm::vec4(1.0F)};
    s.color_curve[1] = {0.1F, glm::vec4(0.5F, 0.5F, 0.5F, 2.0F)};
    s.color_curve[2] = {0.5F, glm::vec4(0.0F)};
    const r::ParticleEmitterSettings c = r::sanitize(s);
    EXPECT_EQ(c.max_particles, 1U);
    EXPECT_FLOAT_EQ(c.lifetime.x, 0.01F);
    EXPECT_FLOAT_EQ(c.lifetime.y, 3.0F);
    EXPECT_EQ(c.direction, glm::vec3(0.0F, 1.0F, 0.0F));
    EXPECT_EQ(c.flipbook_grid, glm::uvec2(4, 1));
    EXPECT_EQ(c.flipbook_frames, 4U);
    EXPECT_FLOAT_EQ(c.color_curve[0].t, 0.1F);
    EXPECT_FLOAT_EQ(c.color_curve[0].color.a, 1.0F); // alpha clamped
    EXPECT_FLOAT_EQ(c.color_curve[1].t, 0.5F);
    EXPECT_FLOAT_EQ(c.color_curve[2].t, 0.9F);
}

[[nodiscard]] r::EmitterGpu record(const r::ParticleEmitterSettings& s, const glm::mat4& world,
                                   float dt = 1.0F / 60.0F) {
    r::EmitterFrameInfo info;
    info.world = world;
    info.dt = dt;
    return r::make_emitter_gpu(r::sanitize(s), info);
}

TEST(ParticleRecord, FlagsCurvesAndLocalGravity) {
    r::ParticleEmitterSettings s;
    s.space = r::ParticleSpace::local;
    s.collide = true;
    s.blend = r::ParticleBlend::alpha;
    s.drag = 2.0F;
    s.sort = true;
    s.max_particles = 700;
    const glm::mat4 world = glm::scale(glm::mat4(1.0F), glm::vec3(2.0F));
    const r::EmitterGpu e = record(s, world, 0.5F);
    EXPECT_NE(e.config.y & r::particle_flag_local, 0U);
    EXPECT_NE(e.config.y & r::particle_flag_collide, 0U);
    EXPECT_EQ(e.config.y & r::particle_flag_additive, 0U);
    EXPECT_NE(e.config.y & r::particle_flag_sort, 0U);
    EXPECT_EQ(e.ids.y, 1024U); // 700 rounded up to a power of two
    EXPECT_FLOAT_EQ(e.forces.w, 0.5F);   // 1 / (1 + 2 * 0.5)
    EXPECT_FLOAT_EQ(e.forces.y, -9.81F / 2.0F); // gravity in emitter units
    // Default colour: white fading out.
    EXPECT_FLOAT_EQ(r::particle_color_at(e, 0.5F).a, 0.5F);
    EXPECT_FLOAT_EQ(r::particle_color_at(e, 2.0F).a, 0.0F);
    EXPECT_FLOAT_EQ(r::particle_size_at(e, 0.7F), 1.0F);
}

TEST(ParticleReference, SpawnStaysInsideItsShape) {
    const glm::mat4 world = glm::translate(glm::mat4(1.0F), glm::vec3(1.0F, 2.0F, 3.0F));
    r::ParticleEmitterSettings s;
    s.radius = 0.75F;
    s.box_half_extents = glm::vec3(0.5F, 1.0F, 2.0F);
    s.cone_angle_deg = 20.0F;
    s.spread_deg = 10.0F;
    s.speed = {2.0F, 4.0F};
    for (const r::ParticleShape shape : {r::ParticleShape::point, r::ParticleShape::sphere,
                                         r::ParticleShape::box, r::ParticleShape::cone}) {
        s.shape = shape;
        const r::EmitterGpu e = record(s, world);
        const float angle = glm::radians(shape == r::ParticleShape::cone ? 20.0F : 10.0F);
        std::set<std::uint32_t> randoms;
        for (std::uint32_t i = 0; i < 500; ++i) {
            const r::ParticleGpu p = r::particle_spawn(e, i);
            const glm::vec3 local = p.position - glm::vec3(1.0F, 2.0F, 3.0F);
            const float speed = glm::length(p.velocity);
            EXPECT_GE(speed, 2.0F - 1e-4F);
            EXPECT_LE(speed, 4.0F + 1e-4F);
            switch (shape) {
            case r::ParticleShape::point: EXPECT_LT(glm::length(local), 1e-5F); break;
            case r::ParticleShape::sphere:
                EXPECT_LE(glm::length(local), 0.75F + 1e-5F);
                break;
            case r::ParticleShape::box:
                EXPECT_TRUE(glm::all(glm::lessThanEqual(glm::abs(local),
                                                        glm::vec3(0.5F, 1.0F, 2.0F) + 1e-5F)));
                break;
            case r::ParticleShape::cone:
                EXPECT_NEAR(local.y, 0.0F, 1e-5F); // the base disc, normal to +Y
                EXPECT_LE(glm::length(local), 0.75F + 1e-5F);
                break;
            }
            if (shape != r::ParticleShape::sphere) {
                const float c = glm::dot(p.velocity / speed, glm::vec3(0.0F, 1.0F, 0.0F));
                EXPECT_GE(c, std::cos(angle) - 1e-4F);
            }
            EXPECT_GE(p.lifetime, 1.0F);
            EXPECT_LE(p.lifetime, 2.0F);
            randoms.insert(p.random);
            // Deterministic: the same index gives the same particle.
            const r::ParticleGpu again = r::particle_spawn(e, i);
            EXPECT_EQ(again.position, p.position);
            EXPECT_EQ(again.random, p.random);
        }
        EXPECT_EQ(randoms.size(), 500U);
    }
    // Another seed, other particles.
    r::ParticleEmitterSettings other = s;
    other.seed = 2;
    EXPECT_NE(r::particle_spawn(record(other, world), 0).position,
              r::particle_spawn(record(s, world), 0).position);
}

TEST(ParticleReference, IntegrationAppliesForcesDragAndAge) {
    r::ParticleEmitterSettings s;
    s.drag = 1.0F;
    const r::EmitterGpu e = record(s, glm::mat4(1.0F), 0.1F);
    r::ParticleGpu p;
    p.velocity = glm::vec3(1.0F, 0.0F, 0.0F);
    p.lifetime = 0.25F;
    ASSERT_TRUE(r::particle_integrate(p, e, 0.1F));
    // v = (v + g dt) / (1 + drag dt)
    EXPECT_NEAR(p.velocity.x, 1.0F / 1.1F, 1e-6F);
    EXPECT_NEAR(p.velocity.y, -0.981F / 1.1F, 1e-5F);
    EXPECT_NEAR(p.position.x, 0.1F / 1.1F, 1e-6F);
    ASSERT_TRUE(r::particle_integrate(p, e, 0.1F));
    EXPECT_FALSE(r::particle_integrate(p, e, 0.1F)); // age 0.3 >= 0.25
}

TEST(ParticleReference, WboitWeightFallsWithDepth) {
    EXPECT_GT(r::wboit_weight(1.0F, 1.0F), r::wboit_weight(1.0F, 10.0F));
    EXPECT_GT(r::wboit_weight(1.0F, 10.0F), r::wboit_weight(1.0F, 100.0F));
    EXPECT_FLOAT_EQ(r::wboit_weight(1.0F, 0.0F), 3000.0F);  // clamped near
    EXPECT_FLOAT_EQ(r::wboit_weight(0.5F, 1e4F), 0.5F * 0.01F); // clamped far
}

} // namespace
