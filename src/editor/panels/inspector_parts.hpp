#pragma once

// Inspector building blocks (ROADMAP E8.3) shared by every component editor
// in inspector.cpp: the entity header (name, type icon, id), the component
// section header with its ⋮ actions implemented through the scene codec
// (reset to defaults, copy / paste values as JSON, remove), and the
// searchable "Add Component" popup over scene::component_infos().

#include <expected>
#include <filesystem>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include <entt/entity/entity.hpp>
#include <entt/entity/fwd.hpp>

#include "engine/core/error.hpp"

namespace engine::scene {
class Scene;
}

namespace engine::editor::panels {

struct InspectorState {
    std::filesystem::path project_root; // asset pickers are confined to it
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

// ---- Section actions (testable without a frame) ------------------------------
// Whether ⋮ > Reset applies: the transform, and components the codec can add
// (not animator / mesh, whose values come from an asset).
[[nodiscard]] bool can_reset(std::string_view codec);
// Resets component `codec` to the codec's defaults in place (merge-update):
// the component is never removed, so nothing that depends on it goes with it.
[[nodiscard]] std::expected<void, core::Error>
reset_component(entt::registry& registry, entt::entity entity, std::string_view codec);
// The component's values as JSON text (nullopt when the entity lacks it).
[[nodiscard]] std::optional<std::string>
copy_component(const entt::registry& registry, entt::entity entity, std::string_view codec);
// Applies the copied values (state.clipboard_*) to component `codec`.
[[nodiscard]] std::expected<void, core::Error>
paste_component(entt::registry& registry, entt::entity entity, const InspectorState& state,
                std::string_view codec);
// Removes a section's component through the codec, with what it implies (a
// Primitive's MeshRenderer, an InstancedMesh's parts and scatter). False when
// `codec` is null or the codec removed nothing.
bool remove_section(entt::registry& registry, entt::entity entity, const char* codec);
// Codec names the Add Component popup offers for `entity` under `filter`.
[[nodiscard]] std::vector<std::string_view>
addable_components(const entt::registry& registry, entt::entity entity, std::string_view filter);

// The icon for a codec component name ("point_light" -> lightbulb).
[[nodiscard]] const char* component_icon(std::string_view codec);
// Display title for a codec component name ("point_light" -> "Point Light").
[[nodiscard]] std::string component_title(std::string_view codec);

} // namespace engine::editor::panels
