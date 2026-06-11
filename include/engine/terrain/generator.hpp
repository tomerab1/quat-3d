#pragma once

// TerrainGenerator — seeded procedural heightmap generation (Phase 12, 12.1).
//
// Layered gradient-noise FBM with iq-style domain warping, composed as a
// low-frequency continental mask that blends rolling plains into ridged
// mountains, followed by an optional particle-based hydraulic erosion pass
// (Beyer droplets) that carves gullies and deposits sediment fans. Fully
// deterministic for a given TerrainParams (including erosion — droplets run
// serially from a seeded PCG), so a saved seed always rebuilds the same world.
//
// generate() is pure CPU and safe to call from any thread; the FBM pass
// parallelises across rows internally. generate_async() runs it on a detached
// worker via std::async (the Phase 10 job system will absorb this later).

#include <cstdint>
#include <expected>
#include <future>

#include "engine/core/error.hpp"
#include "engine/terrain/heightmap.hpp"

namespace engine::terrain {

struct TerrainParams {
    std::uint32_t seed = 1337;
    std::uint32_t resolution = 1025; // texels per side (2^n + 1 plays well with LOD)
    float tile_size_m = 2000.0F;     // world extent of one side
    float height_m = 180.0F;         // peak-to-valley scale before erosion

    // FBM shape.
    int   octaves = 7;
    float lacunarity = 2.0F;
    float gain = 0.5F;
    float base_frequency = 3.0F; // noise periods across the tile at octave 0
    float warp_strength = 0.30F; // domain warp amplitude (fraction of tile)

    // Hydraulic erosion (0 droplets disables the pass).
    std::uint32_t erosion_droplets = 120'000;
    std::uint32_t droplet_lifetime = 30;
    float erosion_inertia = 0.05F;
    float erosion_capacity = 4.0F;
    float erosion_deposit_rate = 0.3F;
    float erosion_erode_rate = 0.3F;
    float erosion_evaporate = 0.02F;
};

[[nodiscard]] Heightmap generate(const TerrainParams& params);

[[nodiscard]] std::future<Heightmap> generate_async(TerrainParams params);

// Self-test: determinism (same seed, same map), seed sensitivity, height
// bounds, domain warp changing the field, and erosion measurably smoothing
// (material moved while total height stays plausible).
[[nodiscard]] std::expected<void, core::Error> run_terrain_self_test();

} // namespace engine::terrain
