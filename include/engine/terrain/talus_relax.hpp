#pragma once

// Talus relaxation — thermal erosion run to its steady state (issue #69).
//
// Thermal weathering (Musgrave, Kolb & Mace 1989, "The Synthesis and
// Rendering of Eroded Fractal Terrains") moves material from a cell to each
// lower neighbour whose height difference exceeds the talus slope times
// their distance. Its steady state has no slope steeper than the talus
// angle, but an explicit step moves material by one cell at most, and a
// steep slope relaxes only from its crest and toe inward (the interior of a
// uniform ramp receives as much as it gives). A flank N cells long needs on
// the order of N² steps, so the few hundred thermal steps interleaved with
// the hydraulic simulation leave 55-75° mountain faces standing.
//
// relax_talus() solves for that steady state with a multigrid cascade.
//   1. Restrict: decimate the height field into a pyramid, each level half
//      the resolution of the one below (texel i of a level is texel 2i of
//      the finer one), down to talus_relax_coarsest texels per side.
//   2. Solve the coarsest level (talus_relax_coarsest_sweeps Jacobi sweeps
//      of the thermal transfer at its own spacing, talus limits scaled by
//      2^level), then, level by level up, add the coarser level's correction
//      (relaxed minus restricted) by bilinear prolongation and smooth with
//      talus_relax_smoothing sweeps.
// Material crosses a mountain flank on the coarse levels in a few sweeps;
// the fine sweeps settle what the coarse levels cannot resolve. Each cycle
// repeats the cascade from the current fine field. Three cycles take a 513²
// mountain tile from about 27 % of its texels steeper than 1.1 x talus
// (after 100-400 hydraulic + thermal steps) to about 0.1 %.
//
// Sweep (8-neighbourhood, neighbours outside the grid are skipped):
//   h' = h + rate * sum_j sign(h_j - h) * max(|h_j - h| - limit_j, 0)
// with rate = 1/8, the largest for which every coefficient of the update
// stays non-negative: the sweep is monotone and max-norm non-expansive (no
// overshoot or oscillation) and conserves material exactly up to rounding
// (pairwise-symmetric transfers). Its fixed points are exactly the surfaces
// with every neighbour pair within the talus limit.
//
// CPU / GPU agreement: renderer::TerrainSynthPass records the same schedule
// (talus_schedule) as compute dispatches (terrain_synth.slang relax_*). The
// sweep needs only add, subtract, abs, compares and a multiply by a power of
// two (exact, so a fused multiply-add rounds the same); restriction copies
// texels; prolongation scales sums by 1/2 and 1/4 (exact). The limits are
// computed here once and passed to the GPU. Fed the same input, both sides
// produce the same bits.

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <vector>

namespace engine::terrain {

// Jacobi sweeps per level per cycle; even, so each level's state ends in its
// `work` half after the ping-pong (no copy back).
inline constexpr std::uint32_t talus_relax_smoothing = 8;
static_assert(talus_relax_smoothing % 2 == 0);
// Levels are halved until they are at most this many texels per side.
inline constexpr std::uint32_t talus_relax_coarsest = 9;
// Sweeps on the coarsest level, which is solved (nearly) to convergence: a
// uniform slope only relaxes from its ends, about N² sweeps for N texels.
inline constexpr std::uint32_t talus_relax_coarsest_sweeps = 64;
static_assert(talus_relax_coarsest_sweeps % 2 == 0);
// Upper bound for TerrainParams::grid.relax_cycles where parameters enter
// the engine (each cycle costs about 14 fine-grid sweeps).
inline constexpr std::uint32_t max_talus_relax_cycles = 8;

struct TalusLevel {
    std::uint32_t resolution = 0;     // texels per side
    std::size_t   work = 0;           // offsets (floats) into the pyramid buffer
    std::size_t   swap = 0;           // ping-pong half of the sweeps
    std::size_t   orig = 0;           // restricted state, before this cycle's corrections (k >= 1)
    float         limit_axis = 0.0F;  // talus x spacing, m: axis neighbours
    float         limit_diagonal = 0.0F;
};

struct TalusPyramid {
    std::vector<TalusLevel> levels; // [0] is the tile's own grid
    std::size_t             floats = 0;
};

// Levels and buffer layout for a `resolution`² grid with `cell_m` spacing.
[[nodiscard]] TalusPyramid talus_pyramid(std::uint32_t resolution, float cell_m, float talus);

enum class TalusOpKind : std::uint8_t {
    restrict_level, // dst, orig (resolution) <- src (src_resolution, finer): decimation
    sweep,          // dst <- one Jacobi sweep of src, both `resolution`
    prolong,        // dst (resolution) += bilinear(src - orig) (src_resolution, coarser)
};

struct TalusOp {
    TalusOpKind   kind = TalusOpKind::sweep;
    std::uint32_t resolution = 0;     // grid written (dispatch size)
    std::uint32_t src_resolution = 0; // grid read
    std::size_t   src = 0;
    std::size_t   dst = 0;
    std::size_t   orig = 0;
    float         limit_axis = 0.0F;  // sweep
    float         limit_diagonal = 0.0F;
};

// The ops of `cycles` cascades over `pyramid`, in execution order. Level 0's
// final state is in pyramid.levels[0].work.
[[nodiscard]] std::vector<TalusOp> talus_schedule(const TalusPyramid& pyramid,
                                                  std::uint32_t cycles);

// One op on the pyramid buffer (the CPU reference of the GPU dispatches).
void run_talus_op(std::vector<float>& pyramid, const TalusOp& op);

// Relaxes `heights_m` (resolution², `cell_m` apart) toward the talus slope
// `talus` (tan of the angle) in place, with `cycles` cascades; 0 does
// nothing. Returns false (heights untouched) once `cancel` is set.
bool relax_talus(std::vector<float>& heights_m, std::uint32_t resolution, float cell_m,
                 float talus, std::uint32_t cycles, const std::atomic<bool>* cancel = nullptr);

} // namespace engine::terrain
