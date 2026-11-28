// Inspector section: Patrol Route (ROADMAP S9: one file per component).
// Registered by the generated inspectors.cpp; see inspectors/inspect.hpp.

#include <cstdio>

#include "inspect.hpp"

namespace engine::editor {

template <>
struct ComponentInspector<scene::PatrolRoute> {
    static constexpr const char* title = "Patrol Route";
    static constexpr const char* codec = "patrol";
    static constexpr const char* icon = icon::route;
    static constexpr bool property_grid = true;
    static void draw(entt::registry&, entt::entity, scene::PatrolRoute& route) {
        (void)ui::prop_bool("Loop", route.loop);
        int remove = -1;
        for (int i = 0; i < static_cast<int>(route.points.size()); ++i) {
            ImGui::PushID(i);
            char label[24];
            std::snprintf(label, sizeof(label), "Point %d", i + 1);
            ui::prop_begin(label, nullptr, true, true);
            float* xyz = &route.points[static_cast<std::size_t>(i)].x;
            ImGui::DragFloat3("##pt", xyz, 0.1F);
            ImGui::SameLine(0.0F, ImGui::GetStyle().ItemInnerSpacing.x);
            if (ui::icon_button("remove", icon::x, "Remove this point")) remove = i;
            ImGui::PopID(); // prop_begin's ID
            ImGui::PopID();
        }
        if (remove >= 0) {
            route.points.erase(route.points.begin() + remove);
            route.next = 0;
            route.finished = false;
        }
        if (!route.points.empty()) {
            ui::prop_begin("", nullptr, false, false);
            if (ui::button(ui::label(icon::trash_2, "Clear points").c_str(),
                           ui::ButtonKind::secondary, ImVec2(-1.0F, 0.0F))) {
                route.points.clear();
                route.next = 0;
                route.finished = false;
            }
            (void)ui::prop_end(false, false);
        }
        ui::prop_note("Shift+click the ground in the viewport to add points; Ctrl+click sends "
                      "the agent somewhere once.");
    }
};

void register_patrol_inspector(InspectorRegistry& registry) {
    registry.add({.name = "patrol",
                  .order = 2000,
                  .draw = [](panels::InspectorState& state, entt::registry& r, entt::entity e) {
            inspect<scene::PatrolRoute>(state, r, e, true);
        }});
}

} // namespace engine::editor
