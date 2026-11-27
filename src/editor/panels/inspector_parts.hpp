#pragma once

// Inspector building blocks (ROADMAP E8.3) shared by every component editor
// in inspector.cpp: the entity header (name, type icon, id), the component
// section header with its ⋮ actions implemented through the scene codec
// (reset to defaults, copy / paste values as JSON, remove), and the
// searchable "Add Component" popup over scene::component_infos().

#include <string>
#include <string_view>

#include <entt/entity/entity.hpp>
#include <entt/entity/fwd.hpp>

namespace engine::scene {
class Scene;
}

namespace engine::editor::panels {

struct InspectorState {
    std::string add_filter;     // Add Component search
    std::string clipboard_type; // codec name of the copied component ("" = none)
    std::string clipboard_json; // its values
};

struct SectionResult {
    bool open = false;
    bool removed = false; // the component is gone: do not draw its fields
};

// One component's header. `codec` is the component's scene-codec name (null
// for components without one: no reset / copy / paste then); `enabled` shows
// a toggle when non-null. Applies the chosen ⋮ action before returning.
SectionResult component_section(entt::registry& registry, entt::entity entity,
                                InspectorState& state, const char* codec, const char* icon,
                                const char* title, bool* enabled, bool removable);

// Name field + type icon + entity id, at the top of the inspector.
void entity_header(scene::Scene& scene, entt::entity entity);

// "Add Component" button and its searchable popup (codec components the
// entity does not have yet, with icons and one-line descriptions).
void add_component_button(entt::registry& registry, entt::entity entity, InspectorState& state);

// The icon for a codec component name ("point_light" -> lightbulb).
[[nodiscard]] const char* component_icon(std::string_view codec);
// Display title for a codec component name ("point_light" -> "Point Light").
[[nodiscard]] std::string component_title(std::string_view codec);

} // namespace engine::editor::panels
