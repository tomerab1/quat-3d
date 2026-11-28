// Per-tile application of terrain edit stamps (G19.1), region-limited.
//
// A change in a region R can only alter texels in R itself and, through
// every smooth stamp after it, the texels that stamp's blur reaches from R
// (`iterations` texels per smooth stamp, within that stamp's footprint).
// apply_edits grows the dirty rectangle by exactly that, recomputes a
// working window around it (grown again by the total blur reach, so every
// blur read near the dirty texels is itself exact) from the retained base
// heights, and writes back only the dirty texels. Each texel's value depends
// only on the base heights in its neighbourhood and the stamps, never on
// the window, which is why the result equals a whole-tile recompute.

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <cstring>
#include <limits>
#include <vector>

#include "engine/terrain/edits.hpp"
#include "engine/terrain/heightmap.hpp"

namespace engine::terrain {

namespace {

[[nodiscard]] float lerp(float x, float y, float a) { return x * (1.0F - a) + y * a; }

[[nodiscard]] TexelRect intersect(const TexelRect& a, const TexelRect& b) {
    TexelRect r{std::max(a.x0, b.x0), std::max(a.z0, b.z0), std::min(a.x1, b.x1),
                std::min(a.z1, b.z1)};
    if (r.empty()) return {};
    return r;
}

[[nodiscard]] TexelRect bbox(const TexelRect& a, const TexelRect& b) {
    if (a.empty()) return b;
    if (b.empty()) return a;
    return {std::min(a.x0, b.x0), std::min(a.z0, b.z0), std::max(a.x1, b.x1),
            std::max(a.z1, b.z1)};
}

[[nodiscard]] TexelRect expand(const TexelRect& r, std::uint32_t by, std::uint32_t res) {
    if (r.empty()) return r;
    return {r.x0 > by ? r.x0 - by : 0, r.z0 > by ? r.z0 - by : 0, std::min(r.x1 + by, res),
            std::min(r.z1 + by, res)};
}

// FNV-1a over one cell's heights and paint.
[[nodiscard]] std::uint64_t cell_fingerprint(const Heightmap& map, std::uint32_t cx,
                                             std::uint32_t cz) {
    const std::uint32_t res = map.resolution;
    const std::uint32_t x0 = cx * res / edit_cells_per_side;
    const std::uint32_t x1 = (cx + 1) * res / edit_cells_per_side;
    const std::uint32_t z0 = cz * res / edit_cells_per_side;
    const std::uint32_t z1 = (cz + 1) * res / edit_cells_per_side;
    std::uint64_t h = 0xcbf29ce484222325ULL;
    const auto mix = [&h](std::uint32_t v) {
        for (int b = 0; b < 4; ++b) {
            h ^= (v >> (8 * b)) & 0xFFU;
            h *= 0x100000001b3ULL;
        }
    };
    for (std::uint32_t z = z0; z < z1; ++z) {
        for (std::uint32_t x = x0; x < x1; ++x) {
            const std::size_t i = static_cast<std::size_t>(z) * res + x;
            std::uint32_t bits = 0;
            std::memcpy(&bits, &map.heights[i], sizeof(bits));
            mix(bits);
            mix(map.paint.empty() ? paint_default_texel : map.paint[i]);
        }
    }
    return h;
}

void refresh_cells(Heightmap& map, const TexelRect& rect) {
    const std::uint32_t res = map.resolution;
    const bool all = map.edit_cells.size() != edit_cells_per_side * edit_cells_per_side;
    if (all) map.edit_cells.assign(edit_cells_per_side * edit_cells_per_side, 0);
    for (std::uint32_t cz = 0; cz < edit_cells_per_side; ++cz) {
        const std::uint32_t z0 = cz * res / edit_cells_per_side;
        const std::uint32_t z1 = (cz + 1) * res / edit_cells_per_side;
        for (std::uint32_t cx = 0; cx < edit_cells_per_side; ++cx) {
            const std::uint32_t x0 = cx * res / edit_cells_per_side;
            const std::uint32_t x1 = (cx + 1) * res / edit_cells_per_side;
            const bool touched = x0 < rect.x1 && rect.x0 < x1 && z0 < rect.z1 && rect.z0 < z1;
            if (all || touched) {
                map.edit_cells[cz * edit_cells_per_side + cx] = cell_fingerprint(map, cx, cz);
            }
        }
    }
}

} // namespace

glm::vec2 TileFrame::texel_position(std::uint32_t x, std::uint32_t z) const {
    // World texel lattice, as the generator samples noise (exact int->float
    // and one correctly rounded multiply, so shared tile edges agree).
    const std::int64_t span = static_cast<std::int64_t>(resolution) - 1;
    const std::int64_t gx = static_cast<std::int64_t>(tile.x) * span + x;
    const std::int64_t gz = static_cast<std::int64_t>(tile.y) * span + z;
    const float t = texel_m();
    const float half = 0.5F * tile_size_m;
    return {static_cast<float>(gx) * t - half, static_cast<float>(gz) * t - half};
}

EditRect TileFrame::bounds() const {
    return {texel_position(0, 0), texel_position(resolution - 1, resolution - 1)};
}

TexelRect texel_rect(const TileFrame& frame, const EditRect& rect) {
    if (frame.resolution < 2 || !(frame.tile_size_m > 0.0F)) return {};
    const EditRect tile = frame.bounds();
    if (!tile.overlaps(rect)) return {};
    // Conservative by one texel on each side: a texel just outside `rect`
    // is evaluated too, and stamps leave it unchanged (zero coverage).
    const float inv = 1.0F / frame.texel_m();
    const auto index = [&](float v, float origin) {
        return static_cast<double>((v - origin) * inv);
    };
    const double res = static_cast<double>(frame.resolution);
    const auto lo = [&](double v) {
        return static_cast<std::uint32_t>(std::clamp(std::floor(v) - 1.0, 0.0, res));
    };
    const auto hi = [&](double v) {
        return static_cast<std::uint32_t>(std::clamp(std::ceil(v) + 2.0, 0.0, res));
    };
    return {lo(index(rect.min.x, tile.min.x)), lo(index(rect.min.y, tile.min.y)),
            hi(index(rect.max.x, tile.min.x)), hi(index(rect.max.y, tile.min.y))};
}

TexelRect apply_edits(Heightmap& map, const TileFrame& frame, std::span<const TerrainStamp> stamps,
                      TexelRect dirty) {
    const std::uint32_t res = map.resolution;
    if (!map.valid() || frame.resolution != res) return {};
    dirty = intersect(dirty, whole_tile(frame));
    if (dirty.empty()) return {};

    // The stamps that reach this tile, in order, with their texel footprints.
    const EditRect tile_bounds = frame.bounds();
    std::vector<TerrainStamp> relevant;
    for (const TerrainStamp& s : stamps) {
        if (stamp_bounds(s).overlaps(tile_bounds)) relevant.push_back(s);
    }
    if (relevant.empty() && map.base_heights.empty()) return {}; // never edited
    const PackedStamps packed = pack_stamps(relevant);
    std::vector<TexelRect> footprint(relevant.size());
    for (std::size_t i = 0; i < relevant.size(); ++i) {
        footprint[i] = texel_rect(frame, stamp_bounds(relevant[i]));
    }

    // Grow the dirty region through every smooth stamp's blur reach.
    for (std::size_t i = 0; i < relevant.size(); ++i) {
        if (relevant[i].op != StampOp::smooth) continue;
        const TexelRect spread =
            intersect(expand(dirty, relevant[i].iterations, res), footprint[i]);
        dirty = bbox(dirty, spread);
    }
    // The window: blur reads clamp to it, so its edge ring is inexact and
    // every smooth pass reaching into it carries that one texel further in.
    // Grow it by the passes of every smooth stamp it meets (a fixed point).
    std::uint32_t reach = 0;
    for (bool grew = true; grew;) {
        grew = false;
        std::uint32_t needed = 0;
        const TexelRect probe = expand(dirty, reach, res);
        for (std::size_t i = 0; i < relevant.size(); ++i) {
            if (relevant[i].op == StampOp::smooth && !intersect(probe, footprint[i]).empty()) {
                needed += relevant[i].iterations;
            }
        }
        if (needed > reach) {
            reach = needed;
            grew = expand(dirty, reach, res) != probe;
        }
    }
    const TexelRect window = expand(dirty, reach, res);

    // First edit of this tile: keep the generated heights.
    if (map.base_heights.empty()) map.base_heights = map.heights;
    if (map.paint.empty()) {
        map.paint.assign(static_cast<std::size_t>(res) * res, paint_default_texel);
    }

    const std::uint32_t ww = window.x1 - window.x0;
    const std::uint32_t wh = window.z1 - window.z0;
    std::vector<float> h(static_cast<std::size_t>(ww) * wh);
    std::vector<std::uint32_t> paint(h.size(), paint_default_texel);
    for (std::uint32_t z = 0; z < wh; ++z) {
        const float* src = map.base_heights.data() +
                           static_cast<std::size_t>(window.z0 + z) * res + window.x0;
        std::copy_n(src, ww, h.data() + static_cast<std::size_t>(z) * ww);
    }
    const auto at = [ww](std::uint32_t x, std::uint32_t z) {
        return static_cast<std::size_t>(z) * ww + x;
    };

    std::vector<float> prev;
    for (std::size_t i = 0; i < relevant.size(); ++i) {
        const PackedStamp& s = packed.stamps[i];
        const TexelRect r = intersect(footprint[i], window);
        if (r.empty()) continue;
        if (relevant[i].op != StampOp::smooth) {
            for (std::uint32_t z = r.z0; z < r.z1; ++z) {
                for (std::uint32_t x = r.x0; x < r.x1; ++x) {
                    const std::size_t k = at(x - window.x0, z - window.z0);
                    apply_pointwise(packed, s, frame.texel_position(x, z), h[k], paint[k]);
                }
            }
            continue;
        }
        // Smooth: a 3x3 binomial blur of the previous state, blended by the
        // coverage; tile-border texels are never smoothed (seams), and reads
        // clamp to the window (its edge ring is outside the dirty texels).
        for (std::uint32_t pass = 0; pass < relevant[i].iterations; ++pass) {
            prev = h;
            const auto read = [&](std::int64_t x, std::int64_t z) {
                x = std::clamp<std::int64_t>(x, window.x0, window.x1 - 1);
                z = std::clamp<std::int64_t>(z, window.z0, window.z1 - 1);
                return prev[at(static_cast<std::uint32_t>(x) - window.x0,
                               static_cast<std::uint32_t>(z) - window.z0)];
            };
            for (std::uint32_t z = std::max(r.z0, 1U); z < std::min(r.z1, res - 1); ++z) {
                for (std::uint32_t x = std::max(r.x0, 1U); x < std::min(r.x1, res - 1); ++x) {
                    const glm::vec2 p = frame.texel_position(x, z);
                    if (p.x < s.bounds.x || p.y < s.bounds.y || p.x > s.bounds.z ||
                        p.y > s.bounds.w) {
                        continue;
                    }
                    const float coverage = packed_coverage(packed, s, p);
                    if (coverage <= 0.0F) continue;
                    const auto xi = static_cast<std::int64_t>(x);
                    const auto zi = static_cast<std::int64_t>(z);
                    const float row0 = read(xi - 1, zi - 1) + 2.0F * read(xi, zi - 1) +
                                       read(xi + 1, zi - 1);
                    const float row1 = read(xi - 1, zi) + 2.0F * read(xi, zi) + read(xi + 1, zi);
                    const float row2 = read(xi - 1, zi + 1) + 2.0F * read(xi, zi + 1) +
                                       read(xi + 1, zi + 1);
                    const float blur = (row0 + 2.0F * row1 + row2) * 0.0625F;
                    const std::size_t k = at(x - window.x0, z - window.z0);
                    h[k] = lerp(prev[k], blur, coverage * s.strength);
                }
            }
        }
    }

    // Write back the dirty texels only.
    for (std::uint32_t z = dirty.z0; z < dirty.z1; ++z) {
        for (std::uint32_t x = dirty.x0; x < dirty.x1; ++x) {
            const std::size_t k = at(x - window.x0, z - window.z0);
            const std::size_t t = static_cast<std::size_t>(z) * res + x;
            map.heights[t] = h[k];
            map.paint[t] = paint[k];
        }
    }
    map.min_height = std::numeric_limits<float>::max();
    map.max_height = std::numeric_limits<float>::lowest();
    for (const float v : map.heights) {
        map.min_height = std::min(map.min_height, v);
        map.max_height = std::max(map.max_height, v);
    }
    refresh_cells(map, dirty);
    return dirty;
}

} // namespace engine::terrain
