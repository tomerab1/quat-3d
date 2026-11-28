// Uniform-grid neighbour search for nav agent separation (#84); see
// nav_separation.hpp.
//
// The grid is Teschner et al.'s hashed uniform grid ("Optimized Spatial
// Hashing for Collision Detection of Deformable Objects", VMV 2003), laid out
// as in S. Green's "Particle Simulation using CUDA" (NVIDIA, 2010): entries
// sorted by cell hash, with a start table instead of per-cell lists. Here a
// counting sort does the sorting, in O(n). Cells hash with Knuth's
// multiplicative (Fibonacci) hashing, The Art of Computer Programming,
// vol. 3, section 6.4.
//
// Layout: every cell (cx, cz) is hashed into one of 2^k buckets (at least 2n,
// so most buckets hold one cell). A counting sort over the buckets files the
// agents into one flat array, `entries_`, with `start_[b]` giving where
// bucket b begins. The fill runs in ascending agent order, so each bucket's
// entries stay in agent order. An agent that moves to another cell during
// the tick is appended to its new bucket's overflow chain. `slot_` records
// which entry is live, and old entries are skipped. A bucket can hold
// several cells, so entries are also matched on the exact cell key.

#include "scene/nav_separation.hpp"

#include <algorithm>
#include <cassert>
#include <cmath>
#include <limits>

namespace engine::scene::nav_detail {

namespace {

// Cell coordinates are clamped to +-kCellLimit. Clamping is monotonic, so
// agents within one cell of each other stay within one cell after it. Far
// out, where floats are spaced more than a metre apart, many cells share
// one; that costs extra tests but never misses a neighbour.
constexpr std::int32_t  kCellLimit = (1 << 30) - 1;
constexpr std::uint64_t kNoCell = std::numeric_limits<std::uint64_t>::max();
constexpr double        kInvCell = 1.0 / static_cast<double>(SeparationGrid::kCellSize);

[[nodiscard]] std::int32_t cell_coord(float v) {
    // In double: exact for every float, and the rounding of the product is
    // tiny next to the cell headroom.
    const double c = std::floor(static_cast<double>(v) * kInvCell);
    return static_cast<std::int32_t>(
        std::clamp(c, -static_cast<double>(kCellLimit), static_cast<double>(kCellLimit)));
}

[[nodiscard]] std::uint64_t pack(std::int32_t cx, std::int32_t cz) {
    return (static_cast<std::uint64_t>(static_cast<std::uint32_t>(cx + kCellLimit)) << 32U) |
           static_cast<std::uint32_t>(cz + kCellLimit);
}

[[nodiscard]] std::int32_t unpack_x(std::uint64_t cell) {
    return static_cast<std::int32_t>(cell >> 32U) - kCellLimit;
}

[[nodiscard]] std::int32_t unpack_z(std::uint64_t cell) {
    return static_cast<std::int32_t>(cell & 0xFFFFFFFFU) - kCellLimit;
}

// An agent at a non-finite position is filed nowhere. Its distance to any
// agent is NaN or infinite, so it neither pushes nor is pushed.
[[nodiscard]] std::uint64_t cell_of(const glm::vec2& p) {
    if (!std::isfinite(p.x) || !std::isfinite(p.y)) return kNoCell;
    return pack(cell_coord(p.x), cell_coord(p.y));
}

} // namespace

std::uint32_t SeparationGrid::bucket_of(std::uint64_t cell) const {
    // Fibonacci hashing (Knuth, TAOCP vol. 3, 6.4): multiply by 2^64 / phi
    // and keep the high bits, which mix both coordinates.
    return static_cast<std::uint32_t>((cell * 0x9E3779B97F4A7C15ULL) >> shift_);
}

void SeparationGrid::build(std::span<const glm::vec2> positions) {
    begin();
    for (const glm::vec2& p : positions) add(p);
    finish();
}

void SeparationGrid::finish() {
    assert(pos_.size() < kNoSlot);
    const auto n = static_cast<std::uint32_t>(pos_.size());
    cell_.resize(n);
    home_.resize(n);
    slot_.resize(n);
    overflow_.clear();
    checks_ = 0;

    // At least 2n buckets (a power of two, minimum 16).
    unsigned bits = 4;
    while ((std::uint64_t{1} << bits) < 2ULL * n) ++bits;
    shift_ = 64U - bits;
    const std::uint32_t buckets = 1U << bits;

    // Count per bucket, then prefix-sum into start offsets.
    start_.assign(buckets + 1U, 0U);
    for (std::uint32_t i = 0; i < n; ++i) {
        const std::uint64_t cell = cell_of(pos_[i]);
        cell_[i] = cell;
        home_[i] = cell;
        if (cell == kNoCell) {
            slot_[i] = kNoSlot;
            continue;
        }
        slot_[i] = kInHome;
        ++start_[bucket_of(cell) + 1U];
    }
    for (std::uint32_t b = 0; b < buckets; ++b) start_[b + 1U] += start_[b];

    // Fill in ascending agent order (overflow_head_ is the write cursor
    // until the fill is done).
    entries_.resize(start_[buckets]);
    overflow_head_.assign(start_.begin(), start_.end() - 1);
    for (std::uint32_t i = 0; i < n; ++i) {
        if (slot_[i] == kInHome) entries_[overflow_head_[bucket_of(cell_[i])]++] = i;
    }
    std::fill(overflow_head_.begin(), overflow_head_.end(), kNil);
}

void SeparationGrid::file(std::uint32_t agent, std::uint64_t cell) {
    if (cell == kNoCell) {
        slot_[agent] = kNoSlot;
    } else if (cell == home_[agent]) {
        slot_[agent] = kInHome; // back where it was built: its build entry is live again
    } else {
        const std::uint32_t b = bucket_of(cell);
        const auto entry = static_cast<std::uint32_t>(overflow_.size());
        assert(entry < kNoSlot);
        overflow_.push_back({agent, overflow_head_[b]});
        overflow_head_[b] = entry;
        slot_[agent] = entry;
    }
}

void SeparationGrid::move(std::uint32_t agent, const glm::vec2& pos) {
    pos_[agent] = pos;
    const std::uint64_t cell = cell_of(pos);
    if (cell == cell_[agent]) return;
    cell_[agent] = cell;
    file(agent, cell);
}

void SeparationGrid::test(std::uint32_t self, std::uint32_t other, const glm::vec2& at) {
    if (other == self) return;
    ++checks_;
    // The same expressions as the all-pairs loop, on the same floats.
    const glm::vec2 away = at - pos_[other];
    const float d = glm::length(away);
    if (separation_in_range(d)) hits_.push_back({other, separation_push(away, d)});
}

glm::vec2 SeparationGrid::steer(std::uint32_t agent, const glm::vec2& dir) {
    glm::vec2 steer = dir;
    const std::uint64_t own = cell_[agent];
    if (own == kNoCell) return steer;
    const glm::vec2 at = pos_[agent];
    const std::int32_t cx = unpack_x(own);
    const std::int32_t cz = unpack_z(own);

    hits_.clear();
    for (std::int32_t z = std::max(cz - 1, -kCellLimit); z <= std::min(cz + 1, kCellLimit); ++z) {
        for (std::int32_t x = std::max(cx - 1, -kCellLimit); x <= std::min(cx + 1, kCellLimit);
             ++x) {
            const std::uint64_t cell = pack(x, z);
            const std::uint32_t b = bucket_of(cell);
            for (std::uint32_t k = start_[b]; k < start_[b + 1U]; ++k) {
                const std::uint32_t other = entries_[k];
                if (slot_[other] == kInHome && cell_[other] == cell) test(agent, other, at);
            }
            for (std::uint32_t e = overflow_head_[b]; e != kNil; e = overflow_[e].next) {
                const std::uint32_t other = overflow_[e].agent;
                if (slot_[other] == e && cell_[other] == cell) test(agent, other, at);
            }
        }
    }

    // Sum in agent order, as the all-pairs loop did: float addition is not
    // associative, and the result must match it bit for bit.
    std::sort(hits_.begin(), hits_.end(),
              [](const Hit& a, const Hit& b) { return a.agent < b.agent; });
    for (const Hit& hit : hits_) steer += hit.push;
    return steer;
}

} // namespace engine::scene::nav_detail
