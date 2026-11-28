#pragma once

// The Inspector's per-component sections (ROADMAP S9). Private editor header.
//
// Each section is one file, src/editor/inspectors/<name>.cpp, which
// specialises ComponentInspector<T> for its component and defines
// register_<name>_inspector(InspectorRegistry&). The build generates the
// calls from the directory (inspectors.cpp.in), so a new section is a new
// file. The generic inspect<T> below finds the component on the entity and
// draws its section header (ui::component_header) and, for editors that
// declare `property_grid`, the label / value grid.
//
// ComponentInspector<T> members: `title`; `draw(registry, entity, T&)` or
// `draw(registry, entity, T&, const InspectorState&)`. Optional: `codec`
// (scene-codec name: enables reset / copy / paste / remove through the
// codec), `icon`, `enabled(T&)` (an enable toggle in the header) and
// `property_grid`.

#include <cstdint>
#include <functional>
#include <span>
#include <string_view>
#include <vector>

#include <entt/entity/registry.hpp>
#include <glm/glm.hpp>
#include <imgui.h>

#include "../panels/inspector_parts.hpp"
#include "../ui/icons.hpp"
#include "../ui/metrics.hpp"
#include "../ui/widgets.hpp"
#include "engine/scene/components.hpp"

namespace engine::editor {

template <typename T>
struct ComponentInspector;

// Small read-only / explanatory line under a component's fields.
inline void hint(const char* text) {
    ImGui::PushTextWrapPos(0.0F);
    ui::text_colored(ui::tokens().text_secondary, text);
    ImGui::PopTextWrapPos();
}

// Draws the component's section (ui::component_header with codec-backed
// reset / copy / paste) and its editor. `removable` offers Remove; returns
// false if the component was removed.
template <typename T>
bool inspect(panels::InspectorState& state, entt::registry& registry, entt::entity entity,
             bool removable = false) {
    using I = ComponentInspector<T>;
    auto* component = registry.try_get<T>(entity);
    if (component == nullptr) return true;

    const char* codec_name = nullptr;
    if constexpr (requires { I::codec; }) codec_name = I::codec;
    const char* glyph = icon::box;
    if constexpr (requires { I::icon; }) glyph = I::icon;
    bool* enabled_flag = nullptr;
    if constexpr (requires { I::enabled(*component); }) enabled_flag = I::enabled(*component);

    const panels::SectionResult s = panels::component_section(
        registry, entity, state, codec_name, glyph, I::title, enabled_flag, removable);
    if (s.removed) {
        // Through the codec when there is one: it also removes what the
        // component implies (a Primitive's generated MeshRenderer, an
        // InstancedMesh's parts and scatter), as the remote API does.
        if (!panels::remove_section(registry, entity, codec_name)) registry.remove<T>(entity);
        return false;
    }
    // A reset / paste may have replaced the component.
    component = registry.try_get<T>(entity);
    if (!s.open || component == nullptr) return true;
    ImGui::PushID(I::title);
    ImGui::Indent(ui::scaled(ui::Spacing::xxs));
    bool grid = false;
    if constexpr (requires { I::property_grid; }) grid = I::property_grid;
    // Editors that need the inspector's context (the project root) take it.
    const auto draw_fields = [&] {
        if constexpr (requires { I::draw(registry, entity, *component, state); }) {
            I::draw(registry, entity, *component, state);
        } else {
            I::draw(registry, entity, *component);
        }
    };
    if (grid && ui::begin_property_grid("##grid")) {
        draw_fields();
        ui::end_property_grid();
    } else if (!grid) {
        draw_fields();
    }
    ImGui::Unindent(ui::scaled(ui::Spacing::xxs));
    ImGui::Dummy(ImVec2(0.0F, ui::scaled(ui::Spacing::xxs)));
    ImGui::PopID();
    return true;
}

// One Inspector section, drawn for the selected entity in ascending `order`
// (ties keep registration order). `draw` draws nothing when the entity lacks
// the component.
struct InspectorEntry {
    std::string_view name{}; // the section's file stem (logs, tests)
    std::int32_t     order = 0;
    std::function<void(panels::InspectorState&, entt::registry&, entt::entity)> draw{};
};

class InspectorRegistry {
public:
    void add(InspectorEntry entry);
    [[nodiscard]] std::span<const InspectorEntry> entries() const { return entries_; }

private:
    std::vector<InspectorEntry> entries_;
};

// The Inspector's sections: the engine's (inspectors/*.cpp) from the first call.
[[nodiscard]] const InspectorRegistry& inspector_registry();
// Registers the engine's sections into `registry` (generated).
void register_engine_inspectors(InspectorRegistry& registry);
// The file stems the build generated calls for, sorted (for tests).
[[nodiscard]] std::span<const std::string_view> engine_inspector_sources();

} // namespace engine::editor
