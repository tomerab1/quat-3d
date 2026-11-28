// The uniform-grid neighbour search behind nav agent separation (#84),
// checked bit for bit against the all-pairs loop it replaced.

#include <bit>
#include <cmath>
#include <cstdint>
#include <limits>
#include <random>
#include <vector>

#include <glm/glm.hpp>
#include <gtest/gtest.h>

#include "scene/nav_separation.hpp"
#include "support/alloc_probe.hpp"

namespace {

namespace nd = engine::scene::nav_detail;

// The all-pairs loop the grid replaced: every other agent, in index order,
// at its current position.
glm::vec2 all_pairs_steer(const std::vector<glm::vec2>& pos, std::uint32_t self, glm::vec2 steer) {
    for (std::uint32_t other = 0; other < pos.size(); ++other) {
        if (other == self) continue;
        const glm::vec2 away = pos[self] - pos[other];
        const float d = glm::length(away);
        if (nd::separation_in_range(d)) steer += nd::separation_push(away, d);
    }
    return steer;
}

[[nodiscard]] bool same_bits(const glm::vec2& a, const glm::vec2& b) {
    return std::bit_cast<std::uint32_t>(a.x) == std::bit_cast<std::uint32_t>(b.x) &&
           std::bit_cast<std::uint32_t>(a.y) == std::bit_cast<std::uint32_t>(b.y);
}

// A crowd with the awkward cases mixed in: agents exactly on cell
// boundaries, pairs exactly kSeparation apart (or one float step closer),
// and coincident agents.
std::vector<glm::vec2> make_crowd(std::mt19937& rng, std::size_t n, float density) {
    const float half = 0.5F * std::sqrt(static_cast<float>(n) / density);
    std::uniform_real_distribution<float> coord(-half, half);
    std::uniform_int_distribution<int>    kind(0, 9);
    const float cell = nd::SeparationGrid::kCellSize;
    std::vector<glm::vec2> pos;
    pos.reserve(n);
    for (std::size_t i = 0; i < n; ++i) {
        glm::vec2 p(coord(rng), coord(rng));
        const int k = pos.empty() ? 0 : kind(rng);
        std::uniform_int_distribution<std::size_t> pick(0, pos.empty() ? 0 : pos.size() - 1);
        if (k == 1) { // on a cell corner
            p = glm::round(p / cell) * cell;
        } else if (k == 2) { // on a cell edge
            p.x = std::round(p.x / cell) * cell;
        } else if (k == 3) { // coincident with another agent
            p = pos[pick(rng)];
        } else if (k == 4) { // exactly the separation radius from another
            p = pos[pick(rng)] + glm::vec2(nd::kSeparation, 0.0F);
        } else if (k == 5) { // one float step inside it
            const glm::vec2 o = pos[pick(rng)];
            p = glm::vec2(o.x, std::nextafter(o.y + nd::kSeparation, o.y));
        } else if (k == 6) { // a grid of separation-radius spacing
            p = glm::round(p / nd::kSeparation) * nd::kSeparation;
        }
        pos.push_back(p);
    }
    return pos;
}

// One simulated tick: agents steer in index order, and some of them move
// before the next one steers (as kinematic agents do), across cells, back to
// where they started, or off to a non-finite position.
struct Mismatch {
    std::size_t count = 0;
    std::uint64_t checks = 0;
};

Mismatch run_tick(nd::SeparationGrid& grid, std::vector<glm::vec2>& pos, std::mt19937& rng,
                  float half) {
    std::uniform_real_distribution<float> unit(-1.0F, 1.0F);
    std::uniform_real_distribution<float> coord(-half, half);
    std::uniform_int_distribution<int>    roll(0, 99);
    grid.build(pos);
    const std::vector<glm::vec2> start = pos;
    Mismatch out;
    for (std::uint32_t i = 0; i < pos.size(); ++i) {
        const glm::vec2 dir(unit(rng), unit(rng));
        if (roll(rng) < 70) { // active this tick
            const glm::vec2 expected = all_pairs_steer(pos, i, dir);
            const glm::vec2 got = grid.steer(i, dir);
            if (!same_bits(expected, got)) ++out.count;
        }
        const int r = roll(rng);
        if (r < 40) {
            pos[i] += glm::vec2(unit(rng), unit(rng)) * 0.1F; // a step
        } else if (r < 45) {
            pos[i] = glm::vec2(coord(rng), coord(rng)); // far away
        } else if (r < 48) {
            pos[i] = start[i]; // back to its build cell
        } else if (r < 49) {
            pos[i].x = std::numeric_limits<float>::quiet_NaN();
        } else if (r < 50) {
            pos[i].y = std::numeric_limits<float>::infinity();
        } else {
            continue;
        }
        grid.move(i, pos[i]);
    }
    out.checks = grid.checks();
    // Bring non-finite agents back for the next tick.
    for (glm::vec2& p : pos) {
        if (!std::isfinite(p.x) || !std::isfinite(p.y)) p = glm::vec2(coord(rng), coord(rng));
    }
    return out;
}

TEST(NavSeparationGrid, MatchesAllPairsBitForBit) {
    for (const std::size_t n : {10U, 50U, 200U, 1000U, 2000U}) {
        for (const std::uint32_t seed : {1U, 2U, 3U, 4U, 5U}) {
            for (const float density : {0.3F, 1.5F}) {
                std::mt19937 rng(seed * 7919U + static_cast<std::uint32_t>(n));
                std::vector<glm::vec2> pos = make_crowd(rng, n, density);
                const float half = 0.5F * std::sqrt(static_cast<float>(n) / density);
                nd::SeparationGrid grid;
                for (int tick = 0; tick < 3; ++tick) {
                    const Mismatch m = run_tick(grid, pos, rng, half);
                    ASSERT_EQ(m.count, 0U) << "n=" << n << " seed=" << seed
                                           << " density=" << density << " tick=" << tick;
                }
            }
        }
    }
}

// Far from the origin floats are spaced a metre or more apart and cell
// coordinates clamp; neighbours must still be found, and nothing overflows.
TEST(NavSeparationGrid, MatchesAllPairsAtExtremeCoordinates) {
    std::vector<glm::vec2> pos;
    for (const float base : {0.0F, 1.0e7F, -1.6e7F, 8388608.0F, 1.0e9F, -3.0e9F, 1.0e30F,
                             std::numeric_limits<float>::max(), -std::numeric_limits<float>::max()}) {
        for (int i = 0; i < 6; ++i) {
            const float x = base + static_cast<float>(i) * 0.7F;
            pos.emplace_back(x, base);
            pos.emplace_back(base, x);
            pos.emplace_back(std::nextafter(x, 0.0F), 0.5F * static_cast<float>(i));
        }
    }
    pos.emplace_back(std::numeric_limits<float>::infinity(), 0.0F);
    pos.emplace_back(std::numeric_limits<float>::infinity(), 0.5F);
    pos.emplace_back(-std::numeric_limits<float>::infinity(), 0.0F);
    pos.emplace_back(std::numeric_limits<float>::quiet_NaN(), 0.0F);
    pos.emplace_back(0.0F, std::numeric_limits<float>::quiet_NaN());

    nd::SeparationGrid grid;
    grid.build(pos);
    for (std::uint32_t i = 0; i < pos.size(); ++i) {
        const glm::vec2 dir(0.25F, -0.5F);
        EXPECT_TRUE(same_bits(all_pairs_steer(pos, i, dir), grid.steer(i, dir)))
            << "agent " << i << " at (" << pos[i].x << ", " << pos[i].y << ")";
    }
    // Agents pushed at all: the test is not vacuous.
    std::size_t pushed = 0;
    for (std::uint32_t i = 0; i < pos.size(); ++i) {
        if (!same_bits(grid.steer(i, glm::vec2(0.0F)), glm::vec2(0.0F))) ++pushed;
    }
    EXPECT_GT(pushed, pos.size() / 4);
}

// Each agent tests only its 3x3 block: the work per agent depends on the
// crowd's density, not its size.
TEST(NavSeparationGrid, WorkGrowsLinearlyWithTheCrowd) {
    double per_agent_small = 0.0;
    for (const std::size_t n : {1250U, 5000U}) {
        std::mt19937 rng(42U);
        const std::vector<glm::vec2> pos = make_crowd(rng, n, 1.0F);
        nd::SeparationGrid grid;
        grid.build(pos);
        for (std::uint32_t i = 0; i < n; ++i) (void)grid.steer(i, glm::vec2(1.0F, 0.0F));
        const double per_agent = static_cast<double>(grid.checks()) / static_cast<double>(n);
        // ~1 agent per m^2 and 9 cells of 1.5625 m^2: ~14 tests per agent
        // (more from the stacked agents make_crowd adds).
        EXPECT_LT(per_agent, 40.0) << n << " agents";
        if (n == 1250U) {
            per_agent_small = per_agent;
        } else {
            EXPECT_LT(per_agent, 1.25 * per_agent_small) << "work per agent grew with n";
            // All pairs would be n - 1 = 4999 per agent.
            EXPECT_LT(grid.checks() * 100U, static_cast<std::uint64_t>(n) * (n - 1));
        }
    }
}

// Rebuilding for a crowd of the same size reuses every buffer.
TEST(NavSeparationGrid, SteadyCrowdDoesNotAllocate) {
    std::mt19937 rng(7U);
    std::vector<glm::vec2> pos = make_crowd(rng, 2000, 1.0F);
    nd::SeparationGrid grid;
    auto tick = [&] {
        grid.build(pos);
        for (std::uint32_t i = 0; i < pos.size(); ++i) {
            (void)grid.steer(i, glm::vec2(1.0F, 0.0F));
            grid.move(i, pos[i] + glm::vec2(3.0F, 0.0F)); // into another cell
        }
    };
    tick(); // warm-up: the buffers grow to size
    quat::test::AllocProbe probe;
    tick();
    tick();
    EXPECT_EQ(probe.count(), 0U);
}

} // namespace
