#pragma once

// Stylised, low-poly nature models for the showcase scenes (conifers,
// broadleaf trees, rocks, a bush), built procedurally from a seed. Sizes are
// in metres with the origin at the base (where the model meets the ground);
// trunks and rocks extend a little below it so they stay seated on slopes.
// Colours are linear, chosen for a natural look under the engine's sky and
// sun rather than for any particular real species.

#include <cstdint>
#include <span>
#include <string_view>

#include "asset-gen/mesh_builder.hpp"

namespace quat::asset_gen {

struct ConiferParams {
    float     height = 12.0F;
    float     radius = 2.8F;       // widest (lowest) tier
    int       tiers = 6;
    float     crown_start = 0.12F; // fraction of the height where the needles begin
    float     overlap = 0.9F;      // tier height = tier spacing * (1 + overlap)
    float     droop = 0.18F;       // rim drop as a fraction of the tier radius
    float     jitter = 0.12F;      // radial noise of the rims
    float     sway = 0.0F;         // random XZ offset of each tier, metres
    float     taper = 0.85F;       // tier radius falloff exponent
    // Detail: sides per tier ring and of the trunk, a mid ring per tier
    // (else a plain cone) and the tier undersides. Distant-forest variants
    // reduce them.
    int       sides = 10;
    int       trunk_sides = 7;
    bool      mid_ring = true;
    bool      underside = true;
    glm::vec3 bark{0.11F, 0.07F, 0.045F};
    glm::vec3 needles_a{0.03F, 0.065F, 0.035F};
    glm::vec3 needles_b{0.045F, 0.085F, 0.038F};
};

struct BroadleafParams {
    float     height = 10.0F;
    float     crown_radius = 3.6F;
    int       branches = 3;
    int       blobs_per_branch = 2;
    float     trunk_radius = 0.32F;
    float     squash = 0.8F; // vertical scale of the canopy blobs
    glm::vec3 bark{0.12F, 0.09F, 0.065F};
    glm::vec3 leaves_a{0.05F, 0.10F, 0.025F};
    glm::vec3 leaves_b{0.075F, 0.13F, 0.03F};
};

struct RockParams {
    glm::vec3 radii{1.2F, 0.9F, 1.0F};
    int       subdivisions = 2;
    float     roughness = 0.22F;
    float     frequency = 1.8F;
    float     embed = 0.2F; // fraction of the height below the origin
    glm::vec3 color{0.2F, 0.19F, 0.175F};
};

struct BushParams {
    int       blobs = 4;
    float     radius = 0.6F;  // typical blob radius
    float     spread = 0.65F; // blob centres within this distance of the axis
    glm::vec3 leaves_a{0.04F, 0.085F, 0.022F};
    glm::vec3 leaves_b{0.06F, 0.11F, 0.028F};
};

[[nodiscard]] Model build_conifer(std::string_view name, const ConiferParams& params,
                                  std::uint32_t seed);
[[nodiscard]] Model build_broadleaf(std::string_view name, const BroadleafParams& params,
                                    std::uint32_t seed);
[[nodiscard]] Model build_rock(std::string_view name, const RockParams& params,
                               std::uint32_t seed);
[[nodiscard]] Model build_bush(std::string_view name, const BushParams& params,
                               std::uint32_t seed);

enum class NatureKind : std::uint8_t { conifer, broadleaf, rock, bush };

// One generated file of the catalogue.
struct NatureAsset {
    std::string_view file; // e.g. "conifer_spruce.glb"
    NatureKind       kind;
    Model (*build)(std::uint32_t seed);
};

// Every model the generator writes, in a fixed order.
[[nodiscard]] std::span<const NatureAsset> nature_catalogue();

// The per-asset seed: catalogue entry `index` under the run's `seed`.
[[nodiscard]] std::uint32_t asset_seed(std::uint32_t seed, std::size_t index);

} // namespace quat::asset_gen
