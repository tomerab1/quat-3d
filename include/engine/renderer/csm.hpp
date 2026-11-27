#pragma once

// Cascaded shadow map fitting (ROADMAP R12) — the CPU reference of what
// csm_setup.slang computes on the GPU each frame (lib/csm.slang mirrors these
// functions operation for operation).
//
// Splits: the practical split scheme (Zhang et al. 2006, "Parallel-Split
// Shadow Maps"): split_i = lambda * log_i + (1 - lambda) * linear_i over the
// shadowed view-depth range. With GPU depth bounds that range is this frame's
// min/max geometry depth (Lauritzen et al. 2011, "Sample Distribution Shadow
// Maps"), quantised to 1/8-octave steps so it changes in discrete jumps
// rather than every frame.
//
// Stability (Valient 2008, "Stable Rendering of Cascaded Shadow Maps";
// MJP's "A Sampling of Shadow Techniques"): each cascade is the bounding
// sphere of its frustum slice, so its size does not change as the camera
// rotates; the radius is rounded up to 1/16 of its power of two (identical
// texel size frame to frame), and the sphere centre is snapped to whole
// shadow texels in a light basis that depends on the light direction alone,
// so a static world point always lands on the same texel position while the
// camera moves.

#include <array>
#include <cstdint>

#include <glm/glm.hpp>

#include "engine/renderer/shadow_ao_settings.hpp"

namespace engine::renderer {

inline constexpr std::uint32_t max_cascades = 4;

// The camera and sun a frame's cascades are fitted to. Use the UNjittered
// camera matrices (TAA jitter would shake the cascades).
struct CsmView {
    glm::mat4 view{1.0F};       // world -> view
    glm::mat4 projection{1.0F}; // view -> clip (Vulkan [0,1] depth)
    glm::vec3 to_light{0.0F, 1.0F, 0.0F}; // unit direction toward the sun
};

// The cascade set as the GPU reads it; mirrors CascadeData in
// lib/csm.slang (scalar layout, 320 bytes).
struct GpuCascades {
    std::array<glm::mat4, max_cascades> view_proj{}; // world -> cascade clip
    glm::vec4 split_far{0.0F};   // view distance where each cascade ends
    glm::vec4 texel_world{0.0F}; // world size of one shadow texel
    glm::vec4 depth_range{0.0F}; // world distance spanned by depth 0..1
    // x = cascade count, y = view distance where cascade 0 starts,
    // z = blend band, w = 1 when the GPU fitted it to this frame's depth
    // bounds (0 from fit_cascades)
    glm::vec4 params{0.0F};
};
static_assert(sizeof(GpuCascades) == 320, "must match lib/csm.slang CascadeData");

// View distance (positive, along -Z of view space) of NDC depth `depth`.
[[nodiscard]] float view_distance_at_depth(const glm::mat4& projection, float depth);

// The practical split distances: [0] = near, [count] = far.
[[nodiscard]] std::array<float, max_cascades + 1>
cascade_splits(float near_dist, float far_dist, std::uint32_t count, float lambda);

// `v` rounded down / up to a 1/8-octave grid of its own magnitude.
[[nodiscard]] float quantize_down(float v);
[[nodiscard]] float quantize_up(float v);

// The shadowed view-distance range for a frame: with valid depth bounds
// (geometry depth min <= max, NDC) the quantised geometry range, otherwise
// camera near .. max_distance. Always within [camera near, max_distance].
[[nodiscard]] glm::vec2 shadow_range(const glm::mat4& projection, const CsmSettings& settings,
                                     bool bounds_valid, float depth_min, float depth_max);

// Fits `settings.cascade_count` cascades over view distances
// [range.x, range.y] for a `map_size`-texel square shadow map per cascade.
[[nodiscard]] GpuCascades fit_cascades(const CsmView& view, const CsmSettings& settings,
                                       glm::vec2 range, std::uint32_t map_size);

// A light-space orthographic volume holding every caster that can shadow a
// visible receiver in any cascade this frame could fit (the whole range
// camera near .. max_distance, extended toward the sun): CPU caster culling
// tests against it, since the GPU-fitted cascades are not known on the CPU.
[[nodiscard]] glm::mat4 caster_cull_view_proj(const CsmView& view, const CsmSettings& settings,
                                              std::uint32_t map_size);

} // namespace engine::renderer
