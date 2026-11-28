#pragma once

// Seeded procedural stones (ROADMAP R23.1): wall stones, cobbles and rocks
// generated at runtime instead of shipped as meshes.
//
// An icosphere scaled to the requested size is displaced by layered value
// noise (fractal sum of octaves, Perlin, "An image synthesizer", SIGGRAPH
// 1985) and then chipped: a few seeded planes cut through the surface and
// vertices beyond a plane are projected onto it, leaving the flat facets of
// split stone. Identical seeds give identical meshes on every platform.

#include <cstdint>
#include <expected>

#include <glm/glm.hpp>

#include "engine/core/error.hpp"
#include "engine/geometry/mesh_data.hpp"

namespace engine::geometry {

struct StoneOptions {
    std::uint64_t seed = 1;
    glm::vec3 size{0.4F, 0.25F, 0.3F}; // full extents (metres) before noise and chips
    std::uint32_t subdivisions = 3;    // icosphere level, 0..5 (20 * 4^n triangles)
    float roundness = 0.6F;            // 0 = box-like (superellipsoid), 1 = ellipsoid
    float noise_amplitude = 0.08F;     // relative to the smallest extent
    std::uint32_t noise_octaves = 3;
    std::uint32_t chips = 3;           // planar cuts, 0..16
    float chip_depth = 0.15F;          // how deep a cut reaches, relative to the size
    float crease_angle_degrees = 35.0F;
    float uv_scale = 0.5F;             // metres per UV unit (box projection)
    std::uint32_t material = 0;
};

[[nodiscard]] std::expected<MeshData, core::Error> generate_stone(const StoneOptions& options = {});

} // namespace engine::geometry
