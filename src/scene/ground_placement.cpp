// Ground placement — see engine/scene/ground_placement.hpp.

#include "engine/scene/ground_placement.hpp"

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <limits>
#include <vector>

namespace engine::scene {

namespace {

// Bound on samples per axis: footprints are a few dozen metres, so this only
// stops an absurd extent / spacing ratio from allocating without limit.
constexpr float k_max_lines_per_axis = 4096.0F;

// `lo`, every lattice line strictly inside (lo, hi), and `hi`.
[[nodiscard]] bool axis_samples(float lo, float hi, float origin, float spacing,
                                std::vector<float>& out) {
    out.clear();
    out.push_back(lo);
    const float first = std::floor((lo - origin) / spacing) + 1.0F;
    const float last = std::ceil((hi - origin) / spacing) - 1.0F;
    if (last - first + 1.0F > k_max_lines_per_axis) return false;
    for (float k = first; k <= last; k += 1.0F) {
        const float v = origin + k * spacing;
        if (v > lo && v < hi) out.push_back(v);
    }
    if (hi > lo) out.push_back(hi);
    return true;
}

[[nodiscard]] bool finite(const glm::vec2& v) { return std::isfinite(v.x) && std::isfinite(v.y); }

} // namespace

std::optional<GroundSpan> ground_span(const GroundHeightFn& ground, const glm::vec2& centre,
                                      const glm::vec2& half_extent,
                                      const GroundLattice& lattice) {
    if (!ground || !(lattice.spacing > 0.0F) || !std::isfinite(lattice.spacing) ||
        !finite(lattice.origin) || !finite(centre) || !finite(half_extent) ||
        half_extent.x < 0.0F || half_extent.y < 0.0F) {
        return std::nullopt;
    }
    const glm::vec2 lo = centre - half_extent;
    const glm::vec2 hi = centre + half_extent;
    std::vector<float> xs;
    std::vector<float> zs;
    if (!axis_samples(lo.x, hi.x, lattice.origin.x, lattice.spacing, xs) ||
        !axis_samples(lo.y, hi.y, lattice.origin.y, lattice.spacing, zs)) {
        return std::nullopt;
    }
    GroundSpan span{std::numeric_limits<float>::max(), std::numeric_limits<float>::lowest()};
    for (const float z : zs) {
        for (const float x : xs) {
            const std::optional<float> h = ground(x, z);
            if (!h || !std::isfinite(*h)) return std::nullopt;
            span.min_y = std::min(span.min_y, *h);
            span.max_y = std::max(span.max_y, *h);
        }
    }
    return span;
}

Pedestal pedestal_on(const GroundSpan& span, float clearance, float embed) {
    return Pedestal{span.max_y + std::max(clearance, 0.0F), span.min_y - std::max(embed, 0.0F)};
}

} // namespace engine::scene
