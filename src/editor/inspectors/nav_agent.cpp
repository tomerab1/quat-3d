// Inspector section: Nav Agent (ROADMAP S9: one file per component).
// Registered by the generated inspectors.cpp; see inspectors/inspect.hpp.

#include "inspect.hpp"
#include "engine/scene/navigation.hpp"

namespace engine::editor {

template <>
struct ComponentInspector<scene::NavAgent> {
    static constexpr const char* title = "Nav Agent";
    static constexpr const char* codec = "nav_agent";
    static constexpr const char* icon = icon::footprints;
    static constexpr bool property_grid = true;
    static void draw(entt::registry& r, entt::entity e, scene::NavAgent& agent) {
        // Editing the target mid-walk is an order: a patrol route waits for it.
        if (ui::prop_vec3("Target", agent.target, {.speed = 0.1F}) && agent.active) {
            agent.commanded = true;
        }
        (void)ui::prop_float("Speed", agent.speed,
                             {.speed = 0.1F, .min = 0.1F, .max = 50.0F, .format = "%.1f m/s"});
        (void)ui::prop_float("Ground offset", agent.ground_offset,
                             {.speed = 0.05F, .min = 0.0F, .max = 10.0F, .format = "%.2f m"},
                             {.tooltip = "Entity origin height above the ground while walking "
                                         "(half-height for a centre-pivot mesh, e.g. 0.5 for the "
                                         "unit cube)"});
        const auto* route = r.try_get<scene::PatrolRoute>(e);
        const bool patrolling = route != nullptr && !route->points.empty();
        const bool running = !agent.paused && (agent.active || patrolling);
        ui::prop_value("State", agent.paused   ? "stopped"
                                : agent.active ? "moving"
                                : agent.arrived ? "arrived"
                                : agent.failed ? "failed (unreachable or stuck)"
                                               : "idle");
        ui::prop_begin("", nullptr, false, false);
        if (ui::button(ui::label(running ? icon::square : icon::play, running ? "Stop" : "Go").c_str(),
                       ui::ButtonKind::secondary, ImVec2(-1.0F, 0.0F))) {
            if (running) {
                scene::nav_agent_stop(agent);
            } else {
                scene::nav_agent_resume(r, e);
            }
        }
        (void)ui::prop_end(false, false);
        ui::prop_note("Bake a navmesh (Physics panel) and press Play; the agent paths to the "
                      "target.");
    }
};

void register_nav_agent_inspector(InspectorRegistry& registry) {
    registry.add({.name = "nav_agent",
                  .order = 1800,
                  .draw = [](panels::InspectorState& state, entt::registry& r, entt::entity e) {
            inspect<scene::NavAgent>(state, r, e, true);
        }});
}

} // namespace engine::editor
