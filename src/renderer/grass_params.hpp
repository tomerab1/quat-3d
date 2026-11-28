#pragma once

// GrassPass internals (R9.3, grass v2): the GPU mirror of GrassParams in
// lib/grass_common.slang and how a frame fills it.

#include <span>

#include <glm/glm.hpp>
#include <vulkan/vulkan.h>

#include "engine/renderer/grass.hpp"
#include "engine/renderer/grass_pass.hpp"
#include "engine/renderer/velocity.hpp"

namespace engine::renderer::grass_detail {

// Mirrors GrassParams in lib/grass_common.slang (std140, 768 bytes).
struct GrassParamsGpu {
    glm::mat4  view_proj{1.0F};
    glm::vec4  planes[6]{};
    glm::vec4  camera{0.0F};
    glm::ivec4 grid{0};
    glm::vec4  cell{0.0F};
    glm::vec4  lod{0.0F};
    glm::vec4  blade{0.0F};
    glm::vec4  limits{0.0F};
    glm::vec4  wind{0.0F};
    glm::vec4  base_color{0.0F};
    glm::vec4  tip_color{0.0F};
    glm::uvec4 counts{0};
    glm::uvec4 draw{0};
    glm::vec4  tile_rect[grass_max_height_tiles]{};
    glm::vec4  tile_info[grass_max_height_tiles]{};
    glm::vec4  ground{0.0F}; // x/y min/max ground y, z slope fade start, w ground cover fade start
    glm::vec4  wind_rect{0.0F}; // wind texture corner XZ, 1 / size, profile at blade height
    glm::vec4  wind_flow{0.0F}; // mean heading XZ (flutter phase), field bound (1) or R9.3 sway (0)
    glm::mat4  prev_view_proj{1.0F};
    glm::vec4  motion{0.0F}; // x previous wind time, yz jitter (NDC)
    // Grass v2.
    glm::vec4  clump{0.0F};       // x size, y pull, z max pull (m), w ground cover fade end (< 0: none)
    glm::vec4  look{0.0F};        // x colour variation, y dry fraction, z root tint, w GBuffer bound
    glm::vec4  dry_color{0.0F};   // rgb, w min blade px
    glm::vec4  footprint{0.0F};   // x pixels per metre at 1 m, y mean width, z/w ground cover near/far
    glm::vec4  cover_color{0.0F}; // rgb thatch colour
    glm::vec4  far_color{0.0F};   // rgb the blades' mean colour
    glm::mat4  inv_view_proj{1.0F};
    glm::vec4  screen{0.0F};      // xy extent, zw 1 / extent
};
static_assert(sizeof(GrassParamsGpu) == 768, "must match grass_common.slang GrassParams");

inline constexpr std::uint32_t near_indices = grass_index_count(grass_near_segments);
inline constexpr std::uint32_t far_indices = grass_index_count(grass_far_segments);

// The blades' mean albedo (root-to-tip ramp, dry clumps mixed in, darkened
// by the field's self-shadowing): what a thinned or distant field averages
// to, and what the ground cover fades to at the radius.
[[nodiscard]] glm::vec3 grass_mean_color(const GrassSettings& settings);

// One frame's constants. `gbuffer_bound` turns on the root tint (the
// generation pass reads the GBuffer).
[[nodiscard]] GrassParamsGpu make_params(const GrassFrame& frame, const GrassSettings& settings,
                                         const GrassGrid& grid, const GrassCapacity& capacity,
                                         std::span<const TerrainHeightTile> tiles,
                                         const ViewMotion& motion, VkExtent2D extent,
                                         bool gbuffer_bound);

} // namespace engine::renderer::grass_detail
