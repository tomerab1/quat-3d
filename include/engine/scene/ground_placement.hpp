#pragma once

// Placing authored content on uneven ground (terrain): the ground's height
// range under a rectangular footprint, and a pedestal that sits on it — its
// top above every ground point under the footprint (nothing pokes through),
// its bottom at or below the lowest one (no gap under any edge).

#include <optional>

#include <glm/glm.hpp>

#include "engine/scene/navigation.hpp" // GroundHeightFn

namespace engine::scene {

// Lowest and highest ground height (world y) under a footprint.
struct GroundSpan {
    float min_y = 0.0F;
    float max_y = 0.0F;
};

// The grid the ground's heights live on: samples every `spacing` metres from
// `origin` (world XZ), e.g. a heightmap's texel lattice.
struct GroundLattice {
    glm::vec2 origin{0.0F};
    float     spacing = 1.0F;
};

// Samples `ground` at every lattice line crossing inside the XZ rectangle
// `centre` ± `half_extent` plus the rectangle's edges at each crossing line
// and its corners. For ground interpolated (bi)linearly between lattice
// points that finds the exact minimum and maximum. nullopt when `ground` is
// empty, any sample has no ground (a tile not loaded, the footprint off the
// terrain), the extent is negative or non-finite, or the spacing is not
// positive.
[[nodiscard]] std::optional<GroundSpan> ground_span(const GroundHeightFn& ground,
                                                    const glm::vec2& centre,
                                                    const glm::vec2& half_extent,
                                                    const GroundLattice& lattice);

// A pedestal over `span`: the top `clearance` metres above the highest ground
// (room for ground cover such as grass), the bottom `embed` metres below the
// lowest so its foot is buried everywhere.
struct Pedestal {
    float top_y = 0.0F;
    float bottom_y = 0.0F;
    [[nodiscard]] float depth() const { return top_y - bottom_y; }
};
[[nodiscard]] Pedestal pedestal_on(const GroundSpan& span, float clearance, float embed);

} // namespace engine::scene
