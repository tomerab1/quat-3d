#pragma once

// The engine's single sub-pixel jitter source (ROADMAP R11). The frame's
// projection, the GBuffer writers' velocity, the temporal resolve and every
// upscaler backend take the jitter from here, so they always agree.
//
// Convention: the jittered view-projection is T(jitter.ndc) * view_proj, i.e.
// a point at unjittered NDC n is rasterised at n + jitter.ndc. Render pixel i
// (centre (i + 0.5) / render in UV) therefore samples the scene point whose
// unjittered UV is (i + 0.5 - jitter.pixels) / render. NDC and UV share the
// y direction (the projection carries Vulkan's y flip), so uv = ndc * 0.5 + 0.5.

#include <cstdint>

#include <glm/glm.hpp>
#include <vulkan/vulkan.h>

namespace engine::renderer {

struct ViewJitter {
    glm::vec2 pixels{0.0F}; // offset in render pixels, each component in [-0.5, 0.5)
    glm::vec2 ndc{0.0F};    // the same offset in NDC: pixels * 2 / render extent
};

// Length of the Halton(2, 3) cycle for a render -> display upscale: 8 phases
// at native resolution, times the pixel-area ratio when upscaling (AMD's FSR 2
// guidance), so every display pixel receives samples over one cycle.
[[nodiscard]] std::uint32_t jitter_phase_count(VkExtent2D render, VkExtent2D display);

// Jitter of frame `frame` (any monotonically increasing counter) at `render`
// extent, cycling through the first `phase_count` Halton(2, 3) points.
[[nodiscard]] ViewJitter halton_jitter(std::uint64_t frame, VkExtent2D render,
                                       std::uint32_t phase_count);

// T(ndc) * view_proj: the matrix rasterisation uses this frame.
[[nodiscard]] glm::mat4 jitter_view_proj(const glm::mat4& view_proj, glm::vec2 ndc);

// Texture LOD bias for rendering at `render` and presenting at `display`:
// log2(render / display) (<= 0), so textures keep display-resolution detail
// after the temporal upscale. Zero at native resolution.
[[nodiscard]] float texture_lod_bias(VkExtent2D render, VkExtent2D display);

} // namespace engine::renderer
