#pragma once

// Scene edits the shell's commands share (create / duplicate / delete),
// used by the menus, the command palette, shortcuts and the hierarchy's
// context menu alike.

#include <span>
#include <string_view>

#include <entt/entity/entity.hpp>
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

// Where new entities appear: `distance` in front of the camera of `view`.
[[nodiscard]] glm::vec3 spawn_point(const glm::mat4& view, float distance = 5.0F);

} // namespace engine::editor::shell
