#include "engine/terrain/budget.hpp"

#include <algorithm>
#include <array>
#include <cstdio>

namespace engine::terrain {

namespace {

constexpr int k_max_radius = 0x7FFF; // terrain_tile_count saturates here

// Resolutions suggested back to the caller: 2^n + 1, which LOD and the
// height-field collider prefer.
constexpr std::array<std::uint32_t, 10> k_resolutions{8193, 4097, 2049, 1025, 513,
                                                      257,  129,  65,   33,   17};

[[nodiscard]] std::string gib(std::uint64_t bytes) {
    std::array<char, 32> text{};
    std::snprintf(text.data(), text.size(), "%.2f GiB",
                  static_cast<double>(bytes) / static_cast<double>(1ULL << 30U));
    return text.data();
}

[[nodiscard]] bool fits(const TerrainParams& params, std::uint32_t tiles, std::uint64_t budget) {
    return terrain_footprint(params, tiles).total() <= budget;
}

} // namespace

std::uint32_t terrain_tile_count(bool streaming, int stream_radius) {
    if (!streaming) return 1;
    // Saturate unvalidated radii (the runtime checks whatever the component
    // holds): side <= 65535, so side² fits 32 bits and is far over any budget.
    const auto r = static_cast<std::uint32_t>(std::clamp(stream_radius, 0, k_max_radius));
    const std::uint32_t side = 2 * r + 1;
    return side * side;
}

TerrainFootprint terrain_footprint(const TerrainParams& params, std::uint32_t tiles) {
    const std::uint64_t res = params.resolution < 2 ? 2 : params.resolution;
    const std::uint64_t texels = res * res;
    std::uint64_t scratch = 0;
    if (erosion_enabled(params)) {
        scratch = params.erosion_model == ErosionModel::grid
                      ? terrain_grid_scratch_bytes_per_texel
                      : terrain_droplet_scratch_bytes_per_texel;
    }
    TerrainFootprint f;
    f.tiles = tiles;
    f.resident_bytes = static_cast<std::uint64_t>(tiles) * texels * terrain_resident_bytes_per_texel;
    f.scratch_bytes = texels * scratch;
    return f;
}

std::optional<TerrainBudgetProblem> terrain_budget_problem(const TerrainParams& params,
                                                           bool streaming, int stream_radius,
                                                           std::uint64_t memory_budget) {
    const std::uint32_t tiles = terrain_tile_count(streaming, stream_radius);
    const TerrainFootprint need = terrain_footprint(params, tiles);
    if (need.total() > memory_budget) {
        std::string m = "terrain would keep " + std::to_string(tiles) + " tile(s) of " +
                        std::to_string(params.resolution) + "^2 texels loaded, about " +
                        gib(need.total()) + " (CPU heightmaps, GPU tiles, physics height " +
                        "fields and generation scratch); the limit is " + gib(memory_budget);
        for (const std::uint32_t res : k_resolutions) {
            TerrainParams smaller = params;
            smaller.resolution = res;
            if (res <= params.resolution && fits(smaller, tiles, memory_budget)) {
                m += ". Use resolution <= " + std::to_string(res);
                if (streaming) m += " at stream_radius " + std::to_string(stream_radius);
                break;
            }
        }
        if (streaming) {
            for (int r = std::min(stream_radius, k_max_radius) - 1; r >= 0; --r) {
                if (fits(params, terrain_tile_count(true, r), memory_budget)) {
                    m += ", or stream_radius <= " + std::to_string(r) + " at resolution " +
                         std::to_string(params.resolution);
                    break;
                }
            }
        }
        return TerrainBudgetProblem{TerrainBudgetProblem::Kind::memory, std::move(m)};
    }
    if (params.erosion_model == ErosionModel::droplets) {
        const std::uint64_t window = static_cast<std::uint64_t>(params.erosion_droplets) * tiles;
        if (window > max_terrain_window_droplets) {
            return TerrainBudgetProblem{
                TerrainBudgetProblem::Kind::droplets,
                "droplets x loaded tiles is " + std::to_string(window) + " (" +
                    std::to_string(tiles) + " tiles); the limit is " +
                    std::to_string(max_terrain_window_droplets) + ". Use droplets <= " +
                    std::to_string(max_terrain_window_droplets / tiles) +
                    ", a smaller stream_radius, or erosion \"grid\" (GPU)"};
        }
    }
    return std::nullopt;
}

} // namespace engine::terrain
