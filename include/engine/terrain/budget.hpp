#pragma once

// Terrain budget — one limit on the combined cost of a terrain configuration
// (issue #67).
//
// Each terrain parameter is range-checked on its own where it enters the
// engine, but the memory a configuration keeps resident is their product: a
// streamed world keeps (2 * stream_radius + 1)² tiles loaded, and every tile
// holds resolution² texels three times over — the CPU Heightmap, the GPU
// tile image and, in Play, the Jolt height field — while one tile at a time
// is generated with its simulation scratch. Droplet erosion's CPU time grows
// with droplets per tile x tiles the same way. terrain_budget_problem()
// checks both before anything is generated; the component codec rejects
// configurations over budget with its message, and the runtime refuses to
// generate them (editor edits do not pass through the codec).

#include <cstdint>
#include <optional>
#include <string>

#include "engine/terrain/generator.hpp"

namespace engine::terrain {

// Resident memory per tile texel: CPU Heightmap (float) + GPU tile image
// (R32F) + Jolt height field (8-bit samples, range blocks and the material
// table, rounded up).
inline constexpr std::uint64_t terrain_resident_bytes_per_texel = 4 + 4 + 2;
// Working set per texel of the one tile being generated. Grid erosion: the
// eroded copy, the simulation state (water, 2x sediment, flux, velocity) and
// the talus-relaxation pyramid on the CPU, or the synthesis scratch, readback
// and image on the GPU, whichever is larger, plus the physics build's padded
// copy. Droplet erosion works on the Heightmap itself.
inline constexpr std::uint64_t terrain_grid_scratch_bytes_per_texel = 80;
inline constexpr std::uint64_t terrain_droplet_scratch_bytes_per_texel = 8;

// Default limit on resident tiles + generation scratch: room for a 1025²
// world at stream_radius 4 (the editor's largest), 2049² at radius 2, or a
// single 8193² tile.
inline constexpr std::uint64_t max_terrain_memory_bytes = 2ULL << 30U; // 2 GiB
// Droplets per tile x tiles kept loaded: the single-tile droplet cap spread
// over a streamed window, so filling the window costs no more CPU time than
// the largest single tile.
inline constexpr std::uint64_t max_terrain_window_droplets = 50'000'000;

// Tiles a configuration keeps loaded: (2r+1)² when streaming, else 1. Radii
// outside [0, 32767] are clamped to it (the count saturates, never wraps).
[[nodiscard]] std::uint32_t terrain_tile_count(bool streaming, int stream_radius);

struct TerrainFootprint {
    std::uint32_t tiles = 0;
    std::uint64_t resident_bytes = 0; // every loaded tile
    std::uint64_t scratch_bytes = 0;  // the tile being generated
    [[nodiscard]] std::uint64_t total() const { return resident_bytes + scratch_bytes; }
};
[[nodiscard]] TerrainFootprint terrain_footprint(const TerrainParams& params, std::uint32_t tiles);

struct TerrainBudgetProblem {
    enum class Kind : std::uint8_t {
        memory,   // resolution x tiles: shrink resolution or stream_radius
        droplets, // droplets x tiles: fewer droplets or a smaller radius
    };
    Kind        kind = Kind::memory;
    std::string message; // what is over, by how much, and the largest values that fit
};

// nullopt when `params` with `streaming` / `stream_radius` fits
// `memory_budget` bytes and the droplet window cap.
[[nodiscard]] std::optional<TerrainBudgetProblem>
terrain_budget_problem(const TerrainParams& params, bool streaming, int stream_radius,
                       std::uint64_t memory_budget = max_terrain_memory_bytes);

} // namespace engine::terrain
