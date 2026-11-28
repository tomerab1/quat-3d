#pragma once

// The per-pixel velocity contract (ROADMAP R11): what the GBuffer's velocity
// target holds, who writes it, and what readers (TAA, upscalers, later motion
// blur and reprojecting effects) may assume.
//
// Format: velocity_format (RG16F), at the render extent, written by every
// opaque GBuffer writer and completed for the background by VelocityPass, so
// every pixel holds a valid value.
//
// Value: v = uv_current - uv_previous of the surface point the pixel shows,
// in UV units (1.0 = the full render target; +x right, +y down). Both UVs are
// computed with UNjittered projections (lib/velocity.slang removes this
// frame's jitter), so a static surface under a static camera has v = 0
// exactly. The previous position of the pixel's surface is uv - v.
//
// Writers and their previous-frame data:
//   * meshes (MeshPass): previous world matrix per instance (MotionHistory,
//     keyed by entity / InstancedMesh entity), and for skinned meshes the
//     positions skinned with last frame's joint palette (SkinningPass);
//   * grass (GrassPass): the blade evaluated at the previous wind time;
//   * terrain (TerrainPass): static, camera motion only;
//   * background (VelocityPass): camera motion of the far plane.
// Fallback: geometry without previous-frame data (a new entity, a draw list
// built without motion, a camera cut) uses its current transform as the
// previous one, so it gets camera-only velocity.

#include <glm/glm.hpp>
#include <vulkan/vulkan.h>

namespace engine::renderer {

inline constexpr VkFormat velocity_format = VK_FORMAT_R16G16_SFLOAT;

// What a GBuffer writer needs to turn positions into velocity: the previous
// frame's unjittered view-projection and this frame's jitter (the rasterised
// clip position minus the jitter is the unjittered current position).
struct ViewMotion {
    glm::mat4 prev_view_proj{1.0F};
    glm::vec2 jitter_ndc{0.0F};

    // No camera motion and no jitter relative to `view_proj` (single frames,
    // tests): only object motion produces velocity.
    [[nodiscard]] static ViewMotion still(const glm::mat4& view_proj) {
        return ViewMotion{view_proj, glm::vec2(0.0F)};
    }
};

// CPU reference of the stored value for a surface point at `world` this frame
// and `prev_world` last frame, with both frames' UNjittered view-projections.
[[nodiscard]] glm::vec2 surface_velocity(const glm::vec3& world, const glm::vec3& prev_world,
                                         const glm::mat4& view_proj,
                                         const glm::mat4& prev_view_proj);

// UV (Vulkan: y down) of a world point under `view_proj`.
[[nodiscard]] glm::vec2 project_to_uv(const glm::vec3& world, const glm::mat4& view_proj);

} // namespace engine::renderer
