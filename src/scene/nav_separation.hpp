#pragma once

// Agent-agent separation for nav_agent_system (#84). Each steering agent is
// pushed away from every other nav agent closer than kSeparation. Finding
// those neighbours by testing every pair cost O(n^2) per tick. Here a uniform
// grid, hashed into a flat table, is rebuilt once per tick in O(n), and each
// agent tests only the agents in its 3x3 block of cells.
//
// Nav runs on the fixed step and feeds network prediction, so the result is
// bit-identical to the all-pairs loop:
// - Cells are a little larger than kSeparation (with headroom for float
//   rounding), so every agent within reach lies in the 3x3 block.
// - Pushes are summed in agent order (the nav view's order), the same order
//   the all-pairs loop used.
// - Agents moved during the tick (kinematic agents step one at a time) are
//   re-filed with move(), so later agents see their new positions.
// The all-pairs loop is kept in nav_agent_system as the reference the tests
// compare against (NavSeparationScratch, through NavSeparationTestAccess).
//
// Techniques (docs/reading/README.md, "Animation, physics & navigation"):
// - Hashed uniform grid for neighbour queries: Teschner et al., "Optimized
//   Spatial Hashing for Collision Detection of Deformable Objects", VMV 2003.
// - Its counting-sort layout (agents sorted by cell hash, plus a cell-start
//   table instead of per-cell lists): S. Green, "Particle Simulation using
//   CUDA", NVIDIA, 2010.
// - Fibonacci (multiplicative) hashing of the cell key: Knuth, The Art of
//   Computer Programming, vol. 3, section 6.4.

#include <cstdint>
#include <span>
#include <vector>

#include <glm/geometric.hpp>
#include <glm/vec2.hpp>

#include "engine/scene/navigation.hpp"

namespace engine::scene::nav_detail {

inline constexpr float kSeparation = 1.2F; // agent-agent comfort radius (m)

// Whether a neighbour at distance `d` pushes. Coincident agents (d ~ 0) have
// no direction to push in.
[[nodiscard]] inline bool separation_in_range(float d) { return d > 1e-4F && d < kSeparation; }

// The push on an agent from one neighbour: `away` = agent - neighbour (xz),
// `d` = |away|. Full strength when touching, fading linearly to 0 at
// kSeparation.
[[nodiscard]] inline glm::vec2 separation_push(const glm::vec2& away, float d) {
    return (away / d) * (1.0F - d / kSeparation);
}

// Agents (xz positions) are indexed 0..n-1 in the order build() received
// them. Buffers are kept across builds, so a steady crowd allocates nothing
// after the first tick.
class SeparationGrid {
public:
    // Cell edge (m). It must be at least kSeparation; the 4% headroom
    // absorbs rounding in the distance and cell computations, so two agents
    // that push each other are never more than one cell apart.
    static constexpr float kCellSize = 1.25F;

    // Filing in three steps, so callers write positions straight into the
    // grid: begin(), add() each agent in order, then finish() files every
    // agent into its cell in O(n + table size), with table size < 4n.
    void begin() { pos_.clear(); }
    void add(const glm::vec2& pos) { pos_.push_back(pos); }
    void finish();
    // begin() + add() of every position + finish().
    void build(std::span<const glm::vec2> positions);

    // Agent `agent` now stands at `pos` (later steer() calls see it there).
    void move(std::uint32_t agent, const glm::vec2& pos);

    // `dir` plus the separation pushes on `agent` from every other agent in
    // range, summed in agent order.
    [[nodiscard]] glm::vec2 steer(std::uint32_t agent, const glm::vec2& dir);

    [[nodiscard]] std::size_t size() const { return pos_.size(); }
    [[nodiscard]] glm::vec2   position(std::uint32_t agent) const { return pos_[agent]; }
    // Candidate distance tests since the last build (the work counter).
    [[nodiscard]] std::uint64_t checks() const { return checks_; }

private:
    struct Hit {
        std::uint32_t agent;
        glm::vec2     push;
    };
    struct Overflow { // an agent moved into another cell after the build
        std::uint32_t agent;
        std::uint32_t next; // next entry in the bucket's chain, or kNil
    };

    static constexpr std::uint32_t kNil = 0xFFFFFFFFU;
    static constexpr std::uint32_t kInHome = 0xFFFFFFFEU; // slot_: its build entry is live
    static constexpr std::uint32_t kNoSlot = 0xFFFFFFFDU; // slot_: in no cell (non-finite position)

    [[nodiscard]] std::uint32_t bucket_of(std::uint64_t cell) const;
    void file(std::uint32_t agent, std::uint64_t cell);
    void test(std::uint32_t self, std::uint32_t other, const glm::vec2& at);

    std::vector<glm::vec2>     pos_;
    std::vector<std::uint64_t> cell_;      // current cell key (kNoCell: non-finite)
    std::vector<std::uint64_t> home_;      // cell at build: where its build entry sits
    std::vector<std::uint32_t> slot_;      // kInHome, kNoSlot or its live overflow entry
    std::vector<std::uint32_t> start_;     // bucket b's build entries: entries_[start_[b], start_[b+1])
    std::vector<std::uint32_t> entries_;   // agent indices, grouped by bucket, ascending
    std::vector<std::uint32_t> overflow_head_; // per bucket, kNil-terminated
    std::vector<Overflow>      overflow_;
    std::vector<Hit>           hits_;
    unsigned                   shift_ = 60; // 64 - log2(bucket count)
    std::uint64_t              checks_ = 0;
};

// nav_agent_system's per-registry scratch, held as a registry context
// variable (registry.ctx()) so the system itself stays stateless.
class NavSeparationScratch {
public:
    SeparationGrid grid; // this tick's agents, in view order

    // Distance tests the last tick spent on separation (either path).
    [[nodiscard]] std::uint64_t neighbour_checks() const {
        return all_pairs_ ? all_pairs_checks_ : grid.checks();
    }

private:
    friend void ::engine::scene::nav_agent_system(entt::registry& registry,
                                                  const nav::NavMesh& navmesh, float dt,
                                                  const GroundHeightFn& ground_height);
    // Tests switch the system to the all-pairs reference (defined in tests/,
    // so shipped code has no way back to O(n^2)).
    friend struct NavSeparationTestAccess;

    bool          all_pairs_ = false;    // steer with the all-pairs loop the grid replaced
    std::uint64_t all_pairs_checks_ = 0; // its distance tests in the last tick
};

} // namespace engine::scene::nav_detail
