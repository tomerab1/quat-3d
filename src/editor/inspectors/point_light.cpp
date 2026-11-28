// Inspector section: Point Light (ROADMAP S9: one file per component).
// Registered by the generated inspectors.cpp; see inspectors/inspect.hpp.

#include "inspect.hpp"

namespace engine::editor {

template <>
struct ComponentInspector<scene::PointLight> {
    static constexpr const char* title = "Point Light";
    static constexpr const char* codec = "point_light";
    static constexpr const char* icon = icon::lightbulb;
    static constexpr bool property_grid = true;
    static void draw(entt::registry&, entt::entity, scene::PointLight& light) {
        (void)ui::prop_color3("Colour", light.color, {.reset = glm::vec3(1.0F)});
        (void)ui::prop_float("Radius", light.radius,
                             {.speed = 0.1F, .min = 0.01F, .max = 1000.0F, .format = "%.2f m"});
        (void)ui::prop_float("Intensity", light.intensity,
                             {.speed = 0.05F, .min = 0.0F, .max = 100.0F});
    }
};

void register_point_light_inspector(InspectorRegistry& registry) {
    registry.add({.name = "point_light",
                  .order = 600,
                  .draw = [](panels::InspectorState& state, entt::registry& r, entt::entity e) {
            inspect<scene::PointLight>(state, r, e, true);
        }});
}

} // namespace engine::editor
