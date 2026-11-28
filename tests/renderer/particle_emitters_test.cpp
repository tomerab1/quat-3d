// ParticleEmitterTable (R10.3): slot and pool-range bookkeeping, re-placement,
// exhaustion, and restarting emitters (editor Play / Stop).

#include <cstdint>
#include <optional>

#include <glm/glm.hpp>
#include <gtest/gtest.h>

#include "engine/renderer/particle_emitters.hpp"
#include "engine/renderer/particles.hpp"

namespace {

namespace r = engine::renderer;
using Table = r::ParticleEmitterTable;

constexpr float k_dt = 1.0F / 60.0F;

[[nodiscard]] r::ParticleEmitterSettings emitter(std::uint32_t capacity) {
    r::ParticleEmitterSettings s;
    s.max_particles = capacity;
    s.rate = 0.0F;
    return s;
}

// One frame visiting `keys` (all with `settings`), committed.
void frame(Table& t, std::initializer_list<std::uint64_t> keys,
           const r::ParticleEmitterSettings& settings) {
    t.begin_frame();
    for (const std::uint64_t k : keys) (void)t.visit(k, settings, glm::mat4(1.0F), k_dt);
    t.end_frame();
    t.commit();
}

TEST(ParticleEmitterTable, DestroyedEmittersFreeTheirSlotAndRangeForReuse) {
    Table t(4096);
    frame(t, {10, 20}, emitter(1024));
    const Table::Entry a = *t.find(10);
    const Table::Entry b = *t.find(20);
    EXPECT_NE(a.slot, b.slot);
    EXPECT_NE(a.base, b.base);
    EXPECT_EQ(t.free_particles(), 4096U - 2048U);

    frame(t, {20}, emitter(1024)); // 10 is gone
    EXPECT_EQ(t.find(10), nullptr);
    EXPECT_EQ(t.free_particles(), 4096U - 1024U);
    EXPECT_EQ(t.free_slots(), r::particle_max_emitters - 1);

    frame(t, {20, 30}, emitter(1024)); // a newcomer takes the freed slot and range
    ASSERT_NE(t.find(30), nullptr);
    EXPECT_EQ(t.find(30)->slot, a.slot);
    EXPECT_EQ(t.find(30)->base, a.base);
    EXPECT_EQ(t.find(20)->base, b.base); // the survivor never moved
}

TEST(ParticleEmitterTable, CapacityOrSpaceChangeMovesTheRangeAndResets) {
    Table t(4096);
    frame(t, {1, 2}, emitter(256)); // 1: [0, 256), 2: [256, 512)
    t.begin_frame();
    const auto settled = t.visit(1, emitter(256), glm::mat4(1.0F), k_dt);
    ASSERT_TRUE(settled);
    EXPECT_FALSE(settled->reset); // committed last frame

    const auto grown = t.visit(1, emitter(1024), glm::mat4(1.0F), k_dt);
    ASSERT_TRUE(grown);
    EXPECT_TRUE(grown->reset);
    EXPECT_EQ(grown->pool_base, 512U); // [0, 256) is too small: first fit after 2
    EXPECT_EQ(grown->spawn_index, 0U); // emission restarted
    EXPECT_EQ(t.find(1)->capacity, 1024U);

    r::ParticleEmitterSettings local = emitter(1024);
    local.space = r::ParticleSpace::local;
    t.commit();
    const auto moved = t.visit(1, local, glm::mat4(1.0F), k_dt);
    ASSERT_TRUE(moved);
    EXPECT_TRUE(moved->reset);
}

TEST(ParticleEmitterTable, ExhaustionWaitsAndRecovers) {
    Table t(1000);
    frame(t, {1}, emitter(600));
    t.begin_frame();
    EXPECT_TRUE(t.visit(1, emitter(600), glm::mat4(1.0F), k_dt));
    EXPECT_FALSE(t.visit(2, emitter(600), glm::mat4(1.0F), k_dt)); // 400 left
    t.end_frame();
    t.commit();
    EXPECT_EQ(t.free_particles(), 400U); // the waiting emitter holds nothing

    t.begin_frame(); // 1 is destroyed: its range frees at the end of this frame
    EXPECT_FALSE(t.visit(2, emitter(600), glm::mat4(1.0F), k_dt));
    t.end_frame();
    t.begin_frame(); // ...so 2 fits on the next one
    const auto placed = t.visit(2, emitter(600), glm::mat4(1.0F), k_dt);
    t.end_frame();
    ASSERT_TRUE(placed);
    EXPECT_TRUE(placed->reset);
    EXPECT_EQ(placed->pool_base, 0U);

    // Slots run out before particles do.
    Table slots(100000);
    slots.begin_frame();
    for (std::uint64_t k = 0; k < r::particle_max_emitters; ++k) {
        ASSERT_TRUE(slots.visit(k, emitter(1), glm::mat4(1.0F), k_dt));
    }
    EXPECT_FALSE(slots.visit(9999, emitter(1), glm::mat4(1.0F), k_dt));
    EXPECT_EQ(slots.free_slots(), 0U);
}

TEST(ParticleEmitterTable, EverythingDestroyedReturnsTheWholePool) {
    Table t(5000);
    frame(t, {1, 2, 3, 4}, emitter(1000));
    frame(t, {2, 4}, emitter(1000));
    frame(t, {}, emitter(1000));
    EXPECT_EQ(t.size(), 0U);
    EXPECT_EQ(t.free_particles(), 5000U);
    EXPECT_EQ(t.free_slots(), r::particle_max_emitters);

    frame(t, {7, 8}, emitter(2500));
    t.reset_all();
    EXPECT_EQ(t.free_particles(), 5000U);
    EXPECT_EQ(t.free_slots(), r::particle_max_emitters);
}

// Editor Play / Stop: a one-shot burst fires again after reset_all.
TEST(ParticleEmitterTable, ResetAllRestartsEmissionAndOneShotBursts) {
    Table t;
    r::ParticleEmitterSettings burst = emitter(256);
    burst.burst_count = 40; // burst_interval 0: once, at start
    const auto run = [&](float dt) {
        t.begin_frame();
        const auto info = t.visit(5, burst, glm::mat4(1.0F), dt);
        t.end_frame();
        t.commit();
        return info;
    };
    auto first = run(k_dt);
    ASSERT_TRUE(first);
    EXPECT_EQ(first->spawn_count, 40U);
    EXPECT_TRUE(first->reset);
    EXPECT_EQ(run(k_dt)->spawn_count, 0U); // fired already
    EXPECT_FALSE(run(k_dt)->reset);

    t.reset_all(); // Stop -> Play
    auto again = run(k_dt);
    ASSERT_TRUE(again);
    EXPECT_EQ(again->spawn_count, 40U);
    EXPECT_TRUE(again->reset);
    EXPECT_EQ(again->spawn_index, 0U);
}

// A frame that failed to record keeps the reset pending.
TEST(ParticleEmitterTable, UncommittedResetsStayPending) {
    Table t;
    t.begin_frame();
    EXPECT_TRUE(t.visit(1, emitter(64), glm::mat4(1.0F), k_dt)->reset);
    t.end_frame(); // no commit: the graph failed
    t.begin_frame();
    EXPECT_TRUE(t.visit(1, emitter(64), glm::mat4(1.0F), k_dt)->reset);
    t.end_frame();
    t.commit();
    t.begin_frame();
    EXPECT_FALSE(t.visit(1, emitter(64), glm::mat4(1.0F), k_dt)->reset);
}

} // namespace
