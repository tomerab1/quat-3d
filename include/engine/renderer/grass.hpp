#pragma once

// GPU grass (ROADMAP R9.3): settings, the camera-centred candidate grid and
// the CPU reference of the per-cell placement and distance LOD that
// grass_generate.slang implements (tests compare the two; see grass_pass.hpp
// for the passes).
//
// Placement: world XZ is divided into square cells of `grass_cell_size()`
// (one candidate blade per cell, i.e. a jittered grid at `density`). Every
// random property of a blade derives from a hash of its world cell index and
// the seed, so blades are fixed in the world: moving the camera only changes
// which cells are evaluated, never where a blade stands (no swimming).
//
// Distance LOD: within `lod_start * radius` every candidate is kept; beyond,
// the kept fraction falls smoothly to `far_density` at the radius, and the
// last `grass_edge_band` of the radius fades to nothing. A blade is kept
// while its random threshold is below the kept fraction and shrinks to zero
// height over the last `grass_fade_band` of thresholds before it is dropped,
// so blades thin out without popping; survivors widen (up to
// `grass_max_width_scale`) to keep the field's coverage. Blades closer than
// `lod_start * radius` draw with `grass_near_segments`, the rest with
// `grass_far_segments`.

#include <array>
#include <cstdint>

#include <glm/glm.hpp>

namespace engine::renderer {

// Every field is in metres / seconds / degrees; colours are linear RGB.
struct GrassSettings {
    std::uint32_t seed = 1;
    float         density = 40.0F; // candidate blades per m^2
    float         radius = 40.0F;  // grass exists within this distance of the camera
    glm::vec2     height_range{0.35F, 0.75F};
    glm::vec2     width_range{0.035F, 0.06F};
    glm::vec3     base_color{0.045F, 0.085F, 0.018F};
    glm::vec3     tip_color{0.30F, 0.36F, 0.10F};
    glm::vec2     wind_direction{1.0F, 0.0F}; // XZ, normalised by sanitize
    float         wind_strength = 0.35F;      // 0 = still; 1 = tips bend ~45 degrees
    float         wind_frequency = 0.5F;      // gust waves per second
    glm::vec2     slope_deg{0.0F, 40.0F};     // grow where the ground slope is in range
    glm::vec2     height_m{-10000.0F, 10000.0F}; // ...and the ground height is in range
    bool          terrain_only = true;        // false: flat ground at ground_y off-terrain
    float         ground_y = 0.0F;
    float         lod_start = 0.3F;   // fraction of radius where thinning begins
    float         far_density = 0.2F; // fraction of candidates kept at the radius
};

inline constexpr std::uint32_t grass_tile_cells = 8; // compute workgroup: 8x8 cells
inline constexpr std::uint32_t grass_near_segments = 7;
inline constexpr std::uint32_t grass_far_segments = 3;
inline constexpr float         grass_edge_band = 0.1F;  // fraction of radius
inline constexpr float         grass_fade_band = 0.15F; // fraction of thresholds
// Below the layer's steepest slope, density thins to zero over this many
// degrees instead of stopping at a hard edge (isolated steep texels then
// thin the field rather than punch bare holes into it).
inline constexpr float         grass_slope_fade_deg = 8.0F;
inline constexpr float         grass_max_width_scale = 2.5F;
// Candidate cells evaluated per frame; denser or larger fields coarsen the grid.
inline constexpr std::uint64_t grass_max_cells = 1ULL << 24;
// Blades one frame can draw (per frame slot; 16 bytes each).
inline constexpr std::uint32_t grass_max_blades = 1U << 21;
inline constexpr std::uint32_t grass_max_height_tiles = 4;

// Clamps every field into its valid range (codec ranges) and normalises the
// wind direction.
[[nodiscard]] GrassSettings sanitize(const GrassSettings& settings);

// Indices of one blade strip (2 * segments + 1 vertices: a pair per level,
// one tip vertex), as a triangle list: (segments - 1) quads + the tip.
[[nodiscard]] constexpr std::uint32_t grass_index_count(std::uint32_t segments) {
    return (segments - 1) * 6 + 3;
}

// The grid of candidate cells around the camera, aligned to world cells.
struct GrassGrid {
    float      cell_size = 0.0F; // metres
    glm::ivec2 origin{0};        // world cell (x, z) of grid cell (0, 0)
    glm::uvec2 cells{0};         // cells per side (x, z), whole tiles
    glm::uvec2 groups{0};        // compute groups (x, z)
};

// 1 / sqrt(density), grown when the disc would exceed grass_max_cells.
[[nodiscard]] float grass_cell_size(const GrassSettings& settings);
[[nodiscard]] GrassGrid grass_grid(const GrassSettings& settings, const glm::vec3& camera);

// Fraction of candidates kept at `distance` from the camera (1 near, falling
// to far_density, then 0 across the outer edge band), and its thinning part
// alone (without the edge fade), which sets the width compensation.
[[nodiscard]] float grass_keep_fraction(const GrassSettings& settings, float distance);
[[nodiscard]] float grass_thinning(const GrassSettings& settings, float distance);

// Ground normal.y at which the slope fade begins (full density at or above
// it, thinning to 0 at the steepest allowed slope's normal.y); -2 when the
// layer allows every slope (no fade). Sent to the GPU as GrassParams.ground.z.
[[nodiscard]] float grass_slope_fade_start(const GrassSettings& settings);
// Fraction of candidates kept on ground whose normal has this y, from the
// slope fade alone (1 on gentle ground, 0 at the steepest allowed slope).
// Multiplies grass_keep_fraction; the hard slope limits still apply.
[[nodiscard]] float grass_slope_keep(const GrassSettings& settings, float normal_y);

// Blade buffer regions: near-LOD blades at [0, near), far-LOD at [near, near + far).
struct GrassCapacity {
    std::uint32_t near = 0;
    std::uint32_t far = 0;
};
[[nodiscard]] GrassCapacity grass_capacity(const GrassSettings& settings);

// pcg3d (Jarzynski & Olano 2020), bit-identical to grass_common.slang.
[[nodiscard]] glm::uvec3 grass_pcg3d(glm::uvec3 v);

// The candidate of world cell (x, z): jittered root position, keep
// threshold in [0, 1) and the 16-bit variation hash the vertex shader
// derives the blade's shape from.
struct GrassCandidate {
    glm::vec2     root_xz{0.0F};
    float         threshold = 0.0F;
    std::uint32_t hash = 0;
};
[[nodiscard]] GrassCandidate grass_candidate(const GrassSettings& settings, float cell_size,
                                             glm::ivec2 cell);

// Height multiplier of a kept blade (0 when dropped): 1 while the threshold
// is well below the kept fraction, falling to 0 as it reaches it.
[[nodiscard]] float grass_blade_fade(float keep, float threshold);

// Gribb-Hartmann planes of a view-projection (Vulkan depth [0, 1]),
// normalised and pointing inward: left, right, bottom, top, near, far.
[[nodiscard]] std::array<glm::vec4, 6> grass_frustum_planes(const glm::mat4& view_proj);

} // namespace engine::renderer
