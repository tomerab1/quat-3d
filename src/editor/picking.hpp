#pragma once

// Viewport picking (editor-private, no ImGui): which mesh a world-space ray
// through the clicked pixel selects.

#include <optional>

#include <entt/entity/fwd.hpp>
#include <glm/glm.hpp>

namespace engine::asset {
struct Aabb;
}

namespace engine::editor {

// Distance along the world ray (`origin`, unit `dir`) to the local-space AABB
// `bounds` placed by `world`; nullopt on a miss (or a box behind the ray). A
// ray that starts inside the box reports where it leaves it: an enclosing
// mesh (a room, a building) then competes with its far side instead of
// winning every click at distance 0.
[[nodiscard]] std::optional<float> pick_distance(const glm::vec3& origin, const glm::vec3& dir,
                                                 const glm::mat4& world,
                                                 const asset::Aabb& bounds);

// The entity whose loaded mesh the ray reaches first (Transform.world +
// MeshRenderer bounds), or entt::null.
[[nodiscard]] entt::entity pick_entity(const entt::registry& registry, const glm::vec3& origin,
                                       const glm::vec3& dir);

} // namespace engine::editor
