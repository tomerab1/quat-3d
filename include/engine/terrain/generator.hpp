#pragma once

// TerrainGenerator — seeded procedural heightmap generation (Phase 12, 12.1).
//
// Layered gradient-noise FBM with iq-style domain warping, composed as a
// low-frequency continental mask that blends rolling plains into ridged
// mountains, followed by an optional erosion pass: particle hydraulic erosion
// (Beyer droplets — CPU only, since each droplet mutates the field the next
// one reads) or grid erosion (pipe-model hydraulic + thermal, the model the
// GPU synthesis pass runs; the CPU version here is its reference). Fully
// deterministic for a given TerrainParams, so a saved seed always rebuilds
// the same world.
//
// Seams: noise is sampled on the world texel lattice — texel x of tile t is
// lattice index g = t * (resolution - 1) + x, at noise position
// float(g) * noise_texel_scale(params) — so a tile's last column and its
// neighbour's first column evaluate the same expression on the same inputs
// (bit-identical on the CPU, and between GPU tiles). Erosion is tile-local
// and faded to zero before the borders.
//
// generate() is pure CPU and safe to call from any thread; the FBM pass and
// the grid erosion parallelise across rows internally. generate_async() runs
// it on a detached worker via std::async. renderer::TerrainSynthPass runs the
// same pipeline in compute shaders (milliseconds instead of seconds a tile).

#include <array>
#include <atomic>
#include <cstdint>
#include <future>
#include <memory>
#include <vector>

#include <glm/glm.hpp>

#include "engine/terrain/heightmap.hpp"

namespace engine::terrain {

// Limits on grid erosion, enforced where parameters enter the engine (the
// component codec): the GPU synthesis pass's largest tile, the editor's step
// cap, and a total texel-step budget (a default 1025² tile at the step cap —
// minutes on a CPU fallback) so no request can wedge a worker for hours or
// allocate gigabytes of simulation state.
inline constexpr std::uint32_t max_grid_erosion_resolution = 2049;
inline constexpr std::uint32_t max_grid_erosion_steps = 5000;
inline constexpr std::uint64_t max_grid_erosion_texel_steps = 1025ULL * 1025ULL * 5000ULL;

// Cooperative cancellation of a CPU generation: once set, generate() stops at
// its next erosion step (or droplet batch) and returns an empty, invalid
// Heightmap.
using CancelToken = std::shared_ptr<std::atomic<bool>>;
[[nodiscard]] inline CancelToken make_cancel_token() {
    return std::make_shared<std::atomic<bool>>(false);
}

enum class ErosionModel : std::uint8_t {
    droplets, // Beyer particle droplets: serial, CPU only
    grid,     // pipe-model hydraulic + thermal grid simulation: GPU, CPU reference
};

// Grid erosion (ErosionModel::grid): the virtual-pipe shallow-water model of
// Mei, Decaudin & Hu 2007 ("Fast Hydraulic Erosion Simulation and
// Visualization on GPU") with a depth-limited sediment capacity (Stava et al.
// 2008), plus thermal weathering that relaxes slopes steeper than the talus
// angle. Runs in metres and seconds on the tile's height grid. Every step
// reads only the previous step's state (no scatter, no atomics), so the
// result is deterministic on the CPU and on the GPU.
struct GridErosionParams {
    std::uint32_t iterations = 200; // simulation steps; 0 disables the pass
    float time_step = 0.05F;        // s per step
    float rain = 0.01F;             // m/s of rain on every cell
    float capacity = 0.5F;          // sediment capacity constant (Kc)
    float erode_rate = 0.3F;        // dissolution rate (Ks), 1/s
    float deposit_rate = 0.5F;      // deposition rate (Kd), 1/s
    float evaporate = 0.3F;         // evaporation rate (Ke), 1/s
    float min_tilt = 0.0F;          // floor on sin(slope): >0 lets flat water carry sediment
    float max_depth = 0.3F;         // m: capacity ramps to full strength at this water depth
    float talus = 0.5F;             // tan of the thermal talus angle (~27 degrees)
    float thermal_rate = 0.5F;      // share of the excess over the talus slope moved per step
};

struct TerrainParams {
    std::uint32_t seed = 1337;
    std::uint32_t resolution = 1025; // texels per side (2^n + 1 plays well with LOD)
    float tile_size_m = 2000.0F;     // world extent of one side
    float height_m = 180.0F;         // peak-to-valley scale before erosion
    // Tile coordinate in an infinite streamed world: the noise field is
    // sampled on the world texel lattice offset by world_tile, so
    // neighbouring tiles generated from the same seed continue each other
    // exactly. (0,0) for a single tile.
    glm::ivec2 world_tile{0, 0};

    // FBM shape.
    int   octaves = 7;
    float lacunarity = 2.0F;
    float gain = 0.5F;
    float base_frequency = 3.0F; // noise periods across the tile at octave 0
    float warp_strength = 0.30F; // domain warp amplitude (fraction of tile)

    ErosionModel erosion_model = ErosionModel::droplets;

    // Droplet erosion (0 droplets disables the pass).
    std::uint32_t erosion_droplets = 120'000;
    std::uint32_t droplet_lifetime = 30;
    float erosion_inertia = 0.05F;
    float erosion_capacity = 4.0F;
    float erosion_deposit_rate = 0.3F;
    float erosion_erode_rate = 0.3F;
    float erosion_evaporate = 0.02F;

    // Grid erosion.
    GridErosionParams grid{};
};

// True when `params` erodes at all (with its selected model).
[[nodiscard]] bool erosion_enabled(const TerrainParams& params);

// Whether `params`' grid erosion is within the limits above (always true for
// droplet erosion or zero steps).
[[nodiscard]] bool grid_erosion_within_limits(const TerrainParams& params);

// `cancel` (optional) aborts the generation: the result is then invalid.
[[nodiscard]] Heightmap generate(const TerrainParams& params,
                                 const std::atomic<bool>* cancel = nullptr);

[[nodiscard]] std::future<Heightmap> generate_async(TerrainParams params,
                                                    CancelToken cancel = {});

// --- Shared with the GPU synthesis pass (renderer::TerrainSynthPass) --------

// The seed's shuffled 256-entry gradient-noise permutation.
[[nodiscard]] std::array<std::uint8_t, 256> noise_permutation(std::uint32_t seed);

// Noise-space distance between neighbouring texels: base_frequency periods
// across the (resolution - 1) texel spans of one tile.
[[nodiscard]] float noise_texel_scale(const TerrainParams& params);

// World texel lattice index of texel `local` of params.world_tile.
[[nodiscard]] glm::ivec2 world_texel(const TerrainParams& params, glm::ivec2 local);

// Tile-local erosion weight at texel (x, z): 0 on the outer border band, whose
// texels keep the pure noise field (so streamed neighbours match exactly),
// ramping to 1 inside. The grid model blends base + fade * (eroded - base).
[[nodiscard]] float grid_erosion_fade(std::uint32_t x, std::uint32_t z,
                                      std::uint32_t resolution);

// Per-step constants of the grid erosion for one cell size. Computed once on
// the CPU and fed to both the CPU reference and the GPU pass, so neither side
// divides where the other multiplies (GPU division is not correctly rounded).
struct GridErosionStep {
    float dt = 0.0F;            // s
    float rain_dt = 0.0F;       // m of rain per step
    float pipe = 0.0F;          // dt * g * pipe area / pipe length (l² / l)
    float area = 0.0F;          // cell area, m²
    float inv_area = 0.0F;
    float cell = 0.0F;          // cell size, m
    float inv_cell = 0.0F;
    float diagonal = 0.0F;      // cell * sqrt(2)
    float advect = 0.0F;        // dt / cell: velocity (m/s) -> cells per step
    float erode_dt = 0.0F;      // Ks * dt
    float deposit_dt = 0.0F;    // Kd * dt
    float keep_water = 0.0F;    // 1 - Ke * dt
    float thermal = 0.0F;       // thermal_rate / 8 neighbours
    float inv_max_depth = 0.0F;
    float capacity = 0.0F;
    float min_tilt = 0.0F;
    float talus = 0.0F;
};
[[nodiscard]] GridErosionStep grid_erosion_step(const GridErosionParams& params, float cell_m);

// Grid erosion of `heights_m` (resolution² metres, cells `cell_m` apart) in
// place, without the border fade.
// Returns false (leaving `heights_m` partly eroded) when `cancel` was set.
bool erode_grid(std::vector<float>& heights_m, std::uint32_t resolution, float cell_m,
                const GridErosionParams& params, const std::atomic<bool>* cancel = nullptr);

} // namespace engine::terrain
