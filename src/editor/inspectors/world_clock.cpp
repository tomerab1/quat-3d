// Inspector section: World Clock (ROADMAP S9: one file per component).
// Registered by the generated inspectors.cpp; see inspectors/inspect.hpp.

#include "inspect.hpp"
#include "../time_of_day_panel.hpp"
#include "engine/scene/world_clock.hpp"

namespace engine::editor {

template <>
struct ComponentInspector<scene::WorldClock> {
    static constexpr const char* title = "World Clock";
    static constexpr const char* codec = "world_clock";
    static constexpr const char* icon = icon::clock_3;
    static constexpr bool property_grid = true;
    static void draw(entt::registry& r, entt::entity e, scene::WorldClock& clock) {
        draw_world_clock(r, e, clock);
    }
};

void register_world_clock_inspector(InspectorRegistry& registry) {
    registry.add({.name = "world_clock",
                  .order = 1500,
                  .draw = [](panels::InspectorState& state, entt::registry& r, entt::entity e) {
            inspect<scene::WorldClock>(state, r, e, true);
        }});
}

} // namespace engine::editor
