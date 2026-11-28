#pragma once

// Hierarchy panel (Phase 9 slice 9.3, redesigned in ROADMAP E8.3): the
// entity tree with type icons, a search filter, inline rename (F2 /
// double-click), drag to re-parent (drop on empty space un-parents), a
// context menu with Create submenus, duplicate and delete, and an empty
// state. Private editor header: included by editor sources only.

#include <string>
#include <string_view>
#include <vector>

#include <entt/entity/entity.hpp>
#include <entt/entity/fwd.hpp>
#include <glm/glm.hpp>

namespace engine::scene {
class Scene;
}

namespace engine::editor {

struct HierarchyState {
    std::string  filter;
    entt::entity renaming = entt::null; // row showing the inline name field
    std::string  rename_text;
    bool         focus_rename = false;
};

// The row label: the Name, or "entity <id>" when it has none or it is empty.
[[nodiscard]] std::string entity_label(const entt::registry& registry, entt::entity e);
// Commits an inline rename, adding a Name when the entity has none.
void commit_rename(entt::registry& registry, entt::entity e, std::string_view text);
// Entities (with a Transform) whose label matches the search `filter`, in
// registry order: the flat list the search shows.
[[nodiscard]] std::vector<entt::entity> filter_entities(const entt::registry& registry,
                                                        std::string_view filter);

// Starts renaming `e` inline (F2 from anywhere routes here).
void begin_rename(HierarchyState& state, const scene::Scene& scene, entt::entity e);

// Draws the Hierarchy window (`window_name`, closable through `open`).
// Selection lives in `selected`; deleting it resets it to entt::null. New
// root entities appear at `spawn_at` (in front of the camera).
void draw_scene_hierarchy(scene::Scene& scene, entt::entity& selected, HierarchyState& state,
                          const glm::vec3& spawn_at, const char* window_name, bool* open);

// The type icon (icon:: constant) the hierarchy shows for `e`, from the
// components it carries (camera, light, terrain, mesh, audio, AI, ...).
[[nodiscard]] const char* entity_type_icon(const entt::registry& registry, entt::entity e);

} // namespace engine::editor
