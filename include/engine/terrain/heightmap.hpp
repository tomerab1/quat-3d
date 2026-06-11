#pragma once

// Heightmap — a square grid of terrain heights in metres (Phase 12, Slice 12.1).
//
// The grid spans tile_size_m on a side in world XZ; texel (0,0) sits at the
// tile's minimum corner. Heights are absolute metres (already scaled by the
// generator). Sampling helpers interpolate bilinearly and derive central-
// difference normals, which the LOD renderer (12.2) and the Jolt height-field
// collider (12.3) both build on.

#include <cstdint>
#include <vector>

#include <glm/glm.hpp>

namespace engine::terrain {

struct Heightmap {
    std::uint32_t      resolution = 0;    // texels per side
    float              tile_size_m = 0.0F; // world extent of one side, metres
    std::vector<float> heights;            // resolution * resolution, metres
    float              min_height = 0.0F;  // tracked by the generator
    float              max_height = 0.0F;

    [[nodiscard]] bool valid() const {
        return resolution > 1 &&
               heights.size() == static_cast<std::size_t>(resolution) * resolution;
    }

    [[nodiscard]] float metres_per_texel() const {
        return tile_size_m / static_cast<float>(resolution - 1);
    }

    // Raw texel access (clamped to the edge).
    [[nodiscard]] float at(std::int32_t x, std::int32_t z) const {
        const auto r = static_cast<std::int32_t>(resolution);
        x = glm::clamp(x, 0, r - 1);
        z = glm::clamp(z, 0, r - 1);
        return heights[static_cast<std::size_t>(z) * resolution + static_cast<std::size_t>(x)];
    }

    // Bilinear height at a local position in metres ([0, tile_size_m]²).
    [[nodiscard]] float sample(float x_m, float z_m) const {
        const float inv = 1.0F / metres_per_texel();
        const float fx = glm::clamp(x_m * inv, 0.0F, static_cast<float>(resolution - 1));
        const float fz = glm::clamp(z_m * inv, 0.0F, static_cast<float>(resolution - 1));
        const auto x0 = static_cast<std::int32_t>(fx);
        const auto z0 = static_cast<std::int32_t>(fz);
        const float tx = fx - static_cast<float>(x0);
        const float tz = fz - static_cast<float>(z0);
        const float h00 = at(x0, z0);
        const float h10 = at(x0 + 1, z0);
        const float h01 = at(x0, z0 + 1);
        const float h11 = at(x0 + 1, z0 + 1);
        return glm::mix(glm::mix(h00, h10, tx), glm::mix(h01, h11, tx), tz);
    }

    // Central-difference normal at a local position in metres (y up).
    [[nodiscard]] glm::vec3 normal(float x_m, float z_m) const {
        const float step = metres_per_texel();
        const float hl = sample(x_m - step, z_m);
        const float hr = sample(x_m + step, z_m);
        const float hd = sample(x_m, z_m - step);
        const float hu = sample(x_m, z_m + step);
        return glm::normalize(glm::vec3(hl - hr, 2.0F * step, hd - hu));
    }
};

} // namespace engine::terrain
