#include "engine/renderer/velocity.hpp"

#include <algorithm>

namespace engine::renderer {

glm::vec2 project_to_uv(const glm::vec3& world, const glm::mat4& view_proj) {
    const glm::vec4 clip = view_proj * glm::vec4(world, 1.0F);
    const glm::vec2 ndc = glm::vec2(clip) / std::max(clip.w, 1e-6F);
    return ndc * 0.5F + 0.5F;
}

glm::vec2 surface_velocity(const glm::vec3& world, const glm::vec3& prev_world,
                           const glm::mat4& view_proj, const glm::mat4& prev_view_proj) {
    return project_to_uv(world, view_proj) - project_to_uv(prev_world, prev_view_proj);
}

} // namespace engine::renderer
