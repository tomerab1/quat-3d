// nav_agent_system's separation grid (#84) against the all-pairs loop it
// replaced, on whole crowds: positions, controller moves and agent state must
// match bit for bit, tick after tick. Nav runs on the fixed step and feeds
// network prediction, so "close" is not enough.

#include <chrono>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <random>
#include <string>
#include <vector>

#include <entt/entity/registry.hpp>
#include <glm/glm.hpp>
#include <gtest/gtest.h>

#include "nav/nav_fixtures.hpp"
#include "scene/nav_separation.hpp"

namespace {

namespace s = engine::scene;
using engine::scene::nav_detail::NavSeparationScratch;
using quat::test::add_box;
using quat::test::kNavDt;

// Builds the same crowd into `w` for the same seed: kinematic walkers and
// CharacterController agents converging on a few gathering points (so they
// crowd), paused, idle and already-arrived agents (obstacles only), some
// standing on grid-cell boundaries or on top of each other.
void populate(quat::test::NavWorld& w, std::size_t n, std::uint32_t seed, float half) {
    add_box(w.registry, glm::vec3(0.0F, -0.5F, 0.0F), glm::vec3(half + 4.0F, 0.5F, half + 4.0F));
    w.bake();
    std::mt19937 rng(seed);
    std::uniform_real_distribution<float> coord(-half, half);
    std::uniform_real_distribution<float> speed(2.0F, 5.0F);
    std::uniform_int_distribution<int>    roll(0, 99);
    const glm::vec3 gather[] = {{0.0F, 0.0F, 0.0F},
                                {0.4F * half, 0.0F, -0.3F * half},
                                {-0.5F * half, 0.0F, 0.5F * half}};
    std::vector<glm::vec3> placed;
    for (std::size_t i = 0; i < n; ++i) {
        glm::vec3 p(coord(rng), 0.0F, coord(rng));
        const int where = roll(rng);
        if (where < 10) { // on a cell corner
            const float cell = engine::scene::nav_detail::SeparationGrid::kCellSize;
            p.x = std::round(p.x / cell) * cell;
            p.z = std::round(p.z / cell) * cell;
        } else if (where < 15 && !placed.empty()) { // on top of another agent
            p = placed[static_cast<std::size_t>(roll(rng)) % placed.size()];
        }
        placed.push_back(p);
        const int kind = roll(rng);
        const bool character = kind >= 50 && kind < 70;
        const entt::entity e = w.agent(character ? p + glm::vec3(0.0F, 0.9F, 0.0F) : p, speed(rng));
        if (character) w.registry.emplace<s::CharacterController>(e);
        s::NavAgent& agent = w.nav(e);
        if (kind < 70) {
            const glm::vec3 target = gather[static_cast<std::size_t>(kind) % 3U] +
                                     glm::vec3(coord(rng), 0.0F, coord(rng)) * 0.1F;
            s::nav_agent_set_target(agent, target);
        } else if (kind < 80) {
            s::nav_agent_set_target(agent, glm::vec3(coord(rng), 0.0F, coord(rng)));
            s::nav_agent_stop(agent); // paused
        } else if (kind < 90) {
            s::nav_agent_set_target(agent, p); // arrives on the first tick
        } // else idle: never sent anywhere
    }
}

// What character_system would do, enough to keep controller agents moving
// (identically in both worlds).
void step_characters(entt::registry& r) {
    for (auto [e, t, cc] : r.view<s::Transform, s::CharacterController>().each()) {
        t.world[3] += glm::vec4(cc.move * (cc.speed * kNavDt), 0.0F);
        t.local = t.world;
    }
}

[[nodiscard]] bool same_bytes(const auto& a, const auto& b) {
    static_assert(sizeof(a) == sizeof(b));
    return std::memcmp(&a, &b, sizeof(a)) == 0;
}

// Empty when the two worlds agree bit for bit, else the first difference.
[[nodiscard]] std::string first_difference(const entt::registry& a, const entt::registry& b) {
    for (auto [e, ta, na] : a.view<const s::Transform, const s::NavAgent>().each()) {
        const auto& tb = b.get<s::Transform>(e);
        const auto& nb = b.get<s::NavAgent>(e);
        const auto id = std::to_string(entt::to_integral(e));
        if (!same_bytes(ta.world, tb.world) || !same_bytes(ta.local, tb.local)) {
            return "transform of agent " + id;
        }
        if (na.active != nb.active || na.arrived != nb.arrived || na.failed != nb.failed ||
            na.paused != nb.paused || na.waypoint != nb.waypoint ||
            na.path.size() != nb.path.size() || !same_bytes(na.progress_best, nb.progress_best) ||
            !same_bytes(na.progress_timer, nb.progress_timer) ||
            na.plan_failures != nb.plan_failures) {
            return "nav state of agent " + id;
        }
        const auto* ca = a.try_get<s::CharacterController>(e);
        const auto* cb = b.try_get<s::CharacterController>(e);
        if ((ca == nullptr) != (cb == nullptr)) return "controller presence of agent " + id;
        if (ca != nullptr && (!same_bytes(ca->move, cb->move) || ca->speed != cb->speed)) {
            return "controller move of agent " + id;
        }
    }
    return {};
}

struct Pair {
    quat::test::NavWorld grid;
    quat::test::NavWorld all_pairs;

    Pair(std::size_t n, std::uint32_t seed, float half) {
        populate(grid, n, seed, half);
        populate(all_pairs, n, seed, half);
        all_pairs.registry.ctx().emplace<NavSeparationScratch>().all_pairs = true;
    }
    void tick() {
        for (auto* w : {&grid, &all_pairs}) {
            s::nav_agent_system(w->registry, w->navmesh, kNavDt);
            step_characters(w->registry);
        }
    }
    [[nodiscard]] std::uint64_t checks(const quat::test::NavWorld& w) const {
        return w.registry.ctx().get<NavSeparationScratch>().neighbour_checks();
    }
};

TEST(NavSeparationCrowd, GridMatchesAllPairsBitForBit) {
    struct Case {
        std::size_t   n;
        std::uint32_t seed;
        float         half; // crowd extent (m)
        int           ticks;
    };
    for (const Case c : {Case{10, 1, 3.0F, 120}, Case{10, 2, 2.0F, 120}, Case{200, 3, 8.0F, 90},
                         Case{200, 4, 14.0F, 90}, Case{800, 5, 16.0F, 40},
                         Case{2000, 6, 25.0F, 12}}) {
        Pair pair(c.n, c.seed, c.half);
        ASSERT_EQ(first_difference(pair.grid.registry, pair.all_pairs.registry), "");
        std::uint64_t grid_checks = 0;
        std::uint64_t all_pairs_checks = 0;
        for (int t = 0; t < c.ticks; ++t) {
            pair.tick();
            grid_checks += pair.checks(pair.grid);
            all_pairs_checks += pair.checks(pair.all_pairs);
            ASSERT_EQ(first_difference(pair.grid.registry, pair.all_pairs.registry), "")
                << c.n << " agents, seed " << c.seed << ", tick " << t;
        }
        // The crowd really moved, and the grid did less work.
        std::size_t moved = 0;
        for (auto [e, t, a] : pair.grid.registry.view<s::Transform, s::NavAgent>().each()) {
            if (a.arrived || !a.path.empty()) ++moved;
        }
        EXPECT_GT(moved, c.n / 3) << c.n << " agents";
        if (c.n >= 200) EXPECT_LT(grid_checks * 4, all_pairs_checks) << c.n << " agents";
    }
}

// The #84 failure scenario: 5,000 agents. One tick tests a bounded number of
// neighbours per agent (O(n k)), not every pair (O(n^2)).
TEST(NavSeparationCrowd, FiveThousandAgentsTickInLinearWork) {
    constexpr std::size_t kAgents = 5000;
    Pair pair(kAgents, 84, 50.0F); // ~0.5 agents per m^2

    const auto start = std::chrono::steady_clock::now();
    s::nav_agent_system(pair.grid.registry, pair.grid.navmesh, kNavDt);
    const auto grid_ms = std::chrono::duration<double, std::milli>(
                             std::chrono::steady_clock::now() - start)
                             .count();
    s::nav_agent_system(pair.all_pairs.registry, pair.all_pairs.navmesh, kNavDt);
    ASSERT_EQ(first_difference(pair.grid.registry, pair.all_pairs.registry), "");

    const std::uint64_t grid = pair.checks(pair.grid);
    const std::uint64_t all_pairs = pair.checks(pair.all_pairs);
    std::printf("[ separation ] %zu agents, one tick: %llu neighbour checks (all pairs: %llu), "
                "%.1f ms\n",
                kAgents, static_cast<unsigned long long>(grid),
                static_cast<unsigned long long>(all_pairs), grid_ms);
    RecordProperty("grid_checks", std::to_string(grid));
    RecordProperty("all_pairs_checks", std::to_string(all_pairs));

    EXPECT_GT(all_pairs, 1000ULL * kAgents) << "too few agents steered for the test to mean much";
    EXPECT_LT(grid, 30ULL * kAgents) << "more than ~30 neighbour checks per agent";
    EXPECT_LT(grid * 100, all_pairs);
    // A hang guard, not a benchmark (the tick includes planning 5,000 paths).
    EXPECT_LT(grid_ms, 10000.0);

    // Steady state: the next ticks (no replanning) cost the same per agent.
    pair.tick();
    EXPECT_LT(pair.checks(pair.grid), 30ULL * kAgents);
    EXPECT_EQ(first_difference(pair.grid.registry, pair.all_pairs.registry), "");
}

} // namespace
