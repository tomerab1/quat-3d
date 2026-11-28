#pragma once

// Scene edits the shell's commands share (create / duplicate / delete),
// used by the menus, the command palette, shortcuts and the hierarchy's
// context menu alike.

#include <cstdint>
#include <span>
#include <string_view>

#include <entt/entity/entity.hpp>
#include <entt/entity/fwd.hpp>
#include <glm/glm.hpp>

namespace engine::scene {
class Scene;
}

namespace engine::editor::shell {

// One "Create" entry: an entity named `name` with the codec component
// `component` (JSON `params`, may be "{}"); an empty component creates a bare
// entity (Name + Transform).
struct CreateKind {
    std::string_view id;        // command id suffix: "create.<id>"
    std::string_view label;     // menu label
    std::string_view group;     // "Shapes", "Lights", ... (menu sections)
    const char*      icon;
    std::string_view name;      // the new entity's Name
    std::string_view component; // codec component name ("" = none)
    std::string_view params;    // codec JSON
};
[[nodiscard]] std::span<const CreateKind> create_kinds();

// Creates `kind` under `parent` (entt::null = root) at world position `at`
// (local to the parent when parented). Returns the entity (entt::null if the
// component could not be applied — logged).
entt::entity create_entity_of(scene::Scene& scene, const CreateKind& kind, entt::entity parent,
                              const glm::vec3& at);

// Deletes `root` and its descendants; clears `selected` if it was among them.
void delete_entity_tree(scene::Scene& scene, entt::entity root, entt::entity& selected);

// `e`'s world matrix from the local transforms up its parent chain (not the
// cached Transform.world, which is only refreshed when the scene ticks).
[[nodiscard]] glm::mat4 current_world(const entt::registry& registry, entt::entity e);

enum class ReparentResult : std::uint8_t {
    done,
    refused,         // invalid entity or a cycle (Scene::set_parent refuses)
    singular_parent, // the parent's world matrix has no inverse (a zero scale)
};

// Re-parents `child` under `parent` (entt::null = root) without moving it:
// its local transform is rewritten so its world pose stays put. Nothing
// changes unless the result is `done`; a parent whose world matrix cannot be
// inverted (zero or non-finite scale) cannot hold the pose and is refused.
[[nodiscard]] ReparentResult reparent_keep_world(scene::Scene& scene, entt::entity child,
                                                 entt::entity parent);

// Where new entities appear: `distance` in front of the camera of `view`.
[[nodiscard]] glm::vec3 spawn_point(const glm::mat4& view, float distance = 5.0F);

} // namespace engine::editor::shell
