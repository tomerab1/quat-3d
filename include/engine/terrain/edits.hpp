#pragma once

// Terrain edit layers (ROADMAP G19.1, docs/terrain-edits.md): an ordered list
// of stamps applied to every generated tile, after generation, in order.
//
// A stamp is a shape — a circle, a capsule along a polyline (a curve sampled
// into points), or a polygon — with an outset `radius` and a `falloff` band
// over which its effect fades to zero (smoothstep of the distance to the
// shape: the signed distance functions of Quilez, "2D distance functions").
// Its operation:
//   flatten   pull heights toward a height, or a plane through it;
//   raise / lower   add / subtract metres;
//   smooth    blend toward a 3x3 binomial blur, `iterations` times;
//   paint     pull one paint channel toward a value.
// Paint channels are material weights (path dirt, gravel, garden soil) and a
// grass-density mask, stored per texel as RGBA8 (`TilePaint`).
//
// Coordinates are terrain-local: metres relative to the Terrain entity's
// position (tile (0,0) is centred on it), heights relative to its Y — the
// frame Heightmap heights live in — so moving the terrain moves its edits.
//
// Determinism: a texel's position is derived from its integer world texel
// index (one int->float conversion and one multiply, as the generator does),
// so the shared edge texels of neighbouring tiles see bit-identical inputs,
// and every pointwise operation is a function of the texel alone. Smoothing
// reads neighbours, so it is pinned to zero on tile-border texels (they stay
// identical between the tiles that share them). Recomputing a region from
// the retained generated heights (`apply_edits`) gives the same bits as
// recomputing the whole tile; tests/terrain/edits_test.cpp checks it, and the
// GPU path (renderer::TerrainEditPass, terrain_edit.slang) against this
// reference.

#include <array>
#include <cstdint>
#include <expected>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

#include <glm/glm.hpp>

namespace engine::terrain {

struct Heightmap;

enum class StampShape : std::uint8_t {
    circle,  // points[0] is the centre; radius is the circle's radius
    capsule, // a polyline (>= 1 point) swept by a disc of `radius`
    polygon, // a simple closed ring (>= 3 points), outset by `radius`
};

enum class StampOp : std::uint8_t {
    flatten, // toward `height` at `pivot`, tilted by `normal` (a plane)
    raise,   // + amount metres
    lower,   // - amount metres
    smooth,  // toward a 3x3 binomial blur, `iterations` times
    paint,   // channel toward `value`
};

enum class PaintChannel : std::uint8_t {
    dirt,   // path dirt
    gravel, // gravel / stone path
    soil,   // garden soil
    grass,  // grass-density mask (1 = full density, 0 = none)
};
inline constexpr std::uint32_t paint_channel_count = 4;

// Limits enforced by validate_stamp (and so by every API and the codec).
inline constexpr std::size_t   max_stamp_points = 256;
inline constexpr std::size_t   max_stamps = 4096;
inline constexpr float         max_stamp_extent_m = 4096.0F; // radius + falloff
inline constexpr float         max_stamp_coordinate_m = 1.0e6F;
inline constexpr float         max_stamp_amount_m = 1000.0F;
inline constexpr std::uint32_t max_smooth_iterations = 16;

struct TerrainStamp {
    std::uint64_t          id = 0; // assigned by TerrainEditList::add; never 0 once listed
    StampShape             shape = StampShape::circle;
    std::vector<glm::vec2> points{}; // terrain-local XZ, metres
    float                  radius = 0.0F;  // circle radius / capsule half-width / polygon outset
    float                  falloff = 0.0F; // metres over which the effect fades to 0
    StampOp                op = StampOp::raise;
    float                  strength = 1.0F; // [0, 1]: blend (flatten, smooth, paint) or scale (raise)
    float                  amount = 0.0F;   // raise / lower, metres (>= 0)
    // flatten: target(x, z) = height + slope . ((x, z) - pivot), with slope
    // derived from `normal` (up = a level height).
    float                  height = 0.0F;
    glm::vec2              pivot{0.0F};
    glm::vec3              normal{0.0F, 1.0F, 0.0F};
    std::uint32_t          iterations = 1; // smooth
    PaintChannel           channel = PaintChannel::dirt;
    float                  value = 1.0F; // paint target, [0, 1]

    friend bool operator==(const TerrainStamp&, const TerrainStamp&) = default;
};

[[nodiscard]] const char* to_string(StampShape shape);
[[nodiscard]] const char* to_string(StampOp op);
[[nodiscard]] const char* to_string(PaintChannel channel);
[[nodiscard]] std::optional<StampShape> stamp_shape_from_string(std::string_view name);
[[nodiscard]] std::optional<StampOp> stamp_op_from_string(std::string_view name);
[[nodiscard]] std::optional<PaintChannel> paint_channel_from_string(std::string_view name);

// Why `stamp` cannot be applied (empty: it can), naming the field.
[[nodiscard]] std::expected<void, std::string> validate_stamp(const TerrainStamp& stamp);

// `stamp` with the fields its op does not read reset to their defaults
// (a paint stamp's height, a flatten stamp's channel...), so equal effects
// compare equal and survive the codec, which writes only the op's fields.
// The list stores stamps in this form.
[[nodiscard]] TerrainStamp canonical_stamp(TerrainStamp stamp);

// Terrain-local XZ rectangle (inclusive).
struct EditRect {
    glm::vec2 min{0.0F};
    glm::vec2 max{0.0F};
    [[nodiscard]] bool overlaps(const EditRect& o) const {
        return min.x <= o.max.x && o.min.x <= max.x && min.y <= o.max.y && o.min.y <= max.y;
    }
    [[nodiscard]] EditRect united(const EditRect& o) const {
        return {glm::min(min, o.min), glm::max(max, o.max)};
    }
};

// Where the stamp can have any effect (shape + radius + falloff).
[[nodiscard]] EditRect stamp_bounds(const TerrainStamp& stamp);

// Weight in [0, 1] of the stamp's shape at terrain-local `p`: 1 inside the
// shape (outset by radius), 0 beyond the falloff band, a smoothstep between.
[[nodiscard]] float stamp_coverage(const TerrainStamp& stamp, glm::vec2 p);

// ---------------------------------------------------------------------------
// The ordered list
// ---------------------------------------------------------------------------

// A process-wide unique, increasing revision number (never 0).
[[nodiscard]] std::uint64_t fresh_edit_revision();

// The edit layer of one terrain: stamps in application order. `revision`
// takes a fresh process-wide value on every mutation (and for every new
// list), so a consumer that remembers the revision it applied notices any
// change, also a list replaced by another. Ids are stable and never reused
// within a list.
class TerrainEditList {
public:
    [[nodiscard]] const std::vector<TerrainStamp>& stamps() const { return stamps_; }
    [[nodiscard]] std::uint64_t revision() const { return revision_; }
    [[nodiscard]] std::uint64_t next_id() const { return next_id_; }

    // Appends (or inserts before the stamp `before`, when given) a validated
    // stamp, assigning a fresh id. Returns the id.
    [[nodiscard]] std::expected<std::uint64_t, std::string>
    add(TerrainStamp stamp, std::optional<std::uint64_t> before = std::nullopt);
    // Replaces stamp `id` in place (keeps its id and position).
    [[nodiscard]] std::expected<void, std::string> update(std::uint64_t id, TerrainStamp stamp);
    // False when no stamp has `id`.
    bool remove(std::uint64_t id);
    void clear();
    [[nodiscard]] const TerrainStamp* find(std::uint64_t id) const;

    // Replaces the whole list (scene load). Every stamp must be valid with a
    // unique non-zero id below `next_id` (0: one past the largest id).
    [[nodiscard]] std::expected<void, std::string> assign(std::vector<TerrainStamp> stamps,
                                                          std::uint64_t next_id = 0);

private:
    std::vector<TerrainStamp> stamps_;
    std::uint64_t             next_id_ = 1;
    std::uint64_t             revision_ = fresh_edit_revision();
};

// Terrain-local rectangles whose ground differs between two versions of a
// list: the old and new bounds of every stamp added, removed, changed or
// moved in the order. Overlapping rectangles are merged.
[[nodiscard]] std::vector<EditRect> changed_regions(std::span<const TerrainStamp> before,
                                                    std::span<const TerrainStamp> after);

// ---------------------------------------------------------------------------
// Per-tile application
// ---------------------------------------------------------------------------

// RGBA8 per texel, channel c in byte c (dirt, gravel, soil, grass mask).
inline constexpr std::uint32_t paint_default_texel = 0xFF000000U; // no material, full grass
[[nodiscard]] inline float paint_channel(std::uint32_t texel, PaintChannel c) {
    return static_cast<float>((texel >> (8U * static_cast<std::uint32_t>(c))) & 0xFFU) / 255.0F;
}

// Where a tile sits: tile (tx, tz) of `tile_size_m`, resolution² texels.
// Texel (x, z) is at world texel index g = tile * (resolution - 1) + (x, z)
// and terrain-local position g * texel_m - tile_size_m / 2.
struct TileFrame {
    glm::ivec2    tile{0, 0};
    std::uint32_t resolution = 0;
    float         tile_size_m = 0.0F;

    [[nodiscard]] float texel_m() const {
        return tile_size_m / static_cast<float>(resolution - 1);
    }
    [[nodiscard]] glm::vec2 texel_position(std::uint32_t x, std::uint32_t z) const;
    [[nodiscard]] EditRect bounds() const;
};

// Half-open texel rectangle [x0, x1) x [z0, z1).
struct TexelRect {
    std::uint32_t x0 = 0, z0 = 0, x1 = 0, z1 = 0;
    [[nodiscard]] bool empty() const { return x1 <= x0 || z1 <= z0; }
    friend bool operator==(const TexelRect&, const TexelRect&) = default;
};

// Texels of `frame` whose positions lie in `rect` (empty when none).
[[nodiscard]] TexelRect texel_rect(const TileFrame& frame, const EditRect& rect);
[[nodiscard]] inline TexelRect whole_tile(const TileFrame& frame) {
    return {0, 0, frame.resolution, frame.resolution};
}

// Revision fingerprints of a tile's edited ground, a grid of
// edit_cells_per_side² cells (scatters regenerate only where theirs change).
inline constexpr std::uint32_t edit_cells_per_side = 16;

// Recomputes the texels of `dirty` (and whatever smoothing spreads them to)
// from `map.base_heights`, applying every stamp of `stamps` in order, and
// writes heights, paint and the touched cells' fingerprints into `map`.
// The first call on a tile moves its generated heights into base_heights.
// Returns the texels that were rewritten (empty when nothing overlaps).
// Equal, bit for bit, to recomputing the whole tile.
TexelRect apply_edits(Heightmap& map, const TileFrame& frame, std::span<const TerrainStamp> stamps,
                      TexelRect dirty);

// The whole tile (a freshly generated or reloaded one).
inline TexelRect apply_edits(Heightmap& map, const TileFrame& frame,
                             std::span<const TerrainStamp> stamps) {
    return apply_edits(map, frame, stamps, whole_tile(frame));
}

// Recomputes every edit-cell fingerprint of `map` (a tile whose edits were
// applied elsewhere: the GPU path).
void fingerprint_edit_cells(Heightmap& map);

// --- Shared with the GPU path (renderer::TerrainEditPass) ------------------

// A stamp in the flattened form both implementations evaluate: per-edge
// constants are computed once here, so neither side divides where the other
// multiplies (GPU division is not correctly rounded).
struct PackedStamp {
    std::uint32_t op = 0;       // StampOp
    std::uint32_t shape = 0;    // StampShape
    std::uint32_t first = 0;    // first point in the shared point / edge arrays
    std::uint32_t count = 0;    // points
    float         radius = 0.0F;
    float         falloff = 0.0F;
    float         inv_falloff = 0.0F; // 0 when falloff == 0
    float         strength = 0.0F;
    float         amount = 0.0F;      // signed: lower is negative
    float         height = 0.0F;
    glm::vec2     pivot{0.0F};
    glm::vec2     slope{0.0F};        // d target / d (x, z)
    std::uint32_t channel = 0;
    float         value = 0.0F;
    std::uint32_t iterations = 0;     // smooth passes
    std::uint32_t pad[3]{};           // std430: bounds starts at 80
    glm::vec4     bounds{0.0F};       // min xz, max xz
};
static_assert(sizeof(PackedStamp) == 96);

// Segment k = points[i] -> points[i + 1] (polygon: wrapping to points[0]):
// xy = edge vector, z = 1 / dot(edge, edge) (0 for a degenerate edge).
struct PackedStamps {
    std::vector<PackedStamp> stamps;
    std::vector<glm::vec2>   points;
    std::vector<glm::vec4>   edges; // one per point (the last unused for a capsule)
};
[[nodiscard]] PackedStamps pack_stamps(std::span<const TerrainStamp> stamps);
[[nodiscard]] float packed_coverage(const PackedStamps& packed, const PackedStamp& s, glm::vec2 p);

// One texel through one pointwise stamp (not smooth). `height` and `paint`
// are updated in place.
void apply_pointwise(const PackedStamps& packed, const PackedStamp& s, glm::vec2 p, float& height,
                     std::uint32_t& paint);

} // namespace engine::terrain
