#pragma once

// GPU form of the layered terrain material (ROADMAP R26.1): the constant
// buffer terrain.slang reads (mirrors TerrainMaterialParams in
// assets/shaders/lib/terrain_material.slang), and the CPU reference of the
// per-tile curvature / flow attributes (terrain_attributes.slang).

#include <array>
#include <cstdint>
#include <span>
#include <string_view>
#include <vector>

#include <glm/glm.hpp>

#include "engine/terrain/material.hpp"

namespace engine::renderer {

struct TerrainLayerGpu {
    glm::vec4 tint_uv{0.0F};   // rgb tint, w = 1 / uv_scale_m
    glm::vec4 slope{0.0F};     // lo, hi, softness, noise
    glm::vec4 altitude{0.0F};  // lo, hi, softness, 1 = snowline-relative
    glm::vec4 affinity{0.0F};  // concavity, flow, strength, paint channel (-1 none, 0..2 rgb)
    glm::vec4 surface{0.0F};   // normal strength, roughness scale, height offset, slice (-1 none)
    glm::vec4 fallback{0.0F};  // untextured colour (linear), roughness
};
static_assert(sizeof(TerrainLayerGpu) == 96, "must match terrain_material.slang");

struct TerrainMaterialGpu {
    glm::vec4 global0{0.0F}; // layer count, blend depth, triplanar start deg, sharpness
    glm::vec4 global1{0.0F}; // macro strength, 1 / macro scale, far start, 1 / far range
    glm::vec4 global2{0.0F}; // stochastic (0/1), snowline (set per frame), mean-colour mip
    std::array<TerrainLayerGpu, terrain::max_material_layers> layers{};
};
static_assert(sizeof(TerrainMaterialGpu) == 816, "must match terrain_material.slang");

// Linear colour and roughness an untextured layer of `texture_set` falls back
// to (a plausible mean of the set's photographs).
[[nodiscard]] glm::vec4 terrain_set_fallback(std::string_view texture_set);

// Packs `material`. `slice_of(layer)` is the texture-array slice of layer i's
// set, or -1 when it has none; `mean_mip` the arrays' 1x1 mip level.
[[nodiscard]] TerrainMaterialGpu pack_terrain_material(const terrain::TerrainMaterial& material,
                                                       std::span<const int> layer_slices,
                                                       float mean_mip);

// CPU reference of terrain_attributes.slang: per texel (r, g) = (concavity
// * 0.5 + 0.5, flow wetness) in [0, 1] after `sweeps` flow sweeps over
// `heights` (resolution², metres, `metres_per_texel` apart).
[[nodiscard]] std::vector<glm::vec2>
terrain_attributes_reference(std::span<const float> heights, std::uint32_t resolution,
                             float metres_per_texel, std::uint32_t sweeps);

// Flow sweeps the GPU bake runs per tile.
inline constexpr std::uint32_t terrain_flow_sweeps = 48;

} // namespace engine::renderer
