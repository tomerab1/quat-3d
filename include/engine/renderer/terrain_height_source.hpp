#pragma once

// TerrainHeightTile — how GPU passes other than the terrain renderer (grass,
// R9.3) sample the terrain surface: one R32F heightmap image per tile, placed
// in world XZ, with the uv convention the terrain pass renders with
// (uv = (world_xz - origin_xz) / size_m over the whole tile, bilinear, clamp
// to edge). Sampling through the same convention puts consumers exactly on
// the rendered surface at every terrain vertex.
//
// TerrainPass::height_tiles() produces these for the CPU-generated tiles;
// any future terrain source (GPU synthesis, clipmaps) hands consumers the
// same description, so they need no knowledge of how the terrain is built.

#include <cstdint>

#include <glm/glm.hpp>
#include <vulkan/vulkan.h>

namespace engine::renderer {

struct TerrainHeightTile {
    VkImageView   view = VK_NULL_HANDLE; // R32F heights (metres), SHADER_READ_ONLY_OPTIMAL
    // RGBA8 edit paint (G19.1): r dirt, g gravel, b soil, a grass-density
    // mask; same uv convention. Always valid for a loaded tile (a 1x1
    // "unpainted" image when the tile has no edits).
    VkImageView   paint = VK_NULL_HANDLE;
    glm::vec2     origin_xz{0.0F};       // world XZ of the tile's minimum corner
    float         size_m = 0.0F;         // world extent of one side
    float         base_y = 0.0F;         // world Y added to every height
    float         min_height = 0.0F;     // height range within the tile (before base_y)
    float         max_height = 0.0F;
    std::uint32_t resolution = 0;        // texels per side
};

} // namespace engine::renderer
