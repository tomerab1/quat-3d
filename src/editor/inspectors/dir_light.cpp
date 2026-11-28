// Inspector section: Directional Light (ROADMAP S9: one file per component).
// Registered by the generated inspectors.cpp; see inspectors/inspect.hpp.

#include "inspect.hpp"

namespace engine::editor {

template <>
struct ComponentInspector<scene::DirectionalLight> {
    static constexpr const char* title = "Directional Light";
    static constexpr const char* codec = "dir_light";
    static constexpr const char* icon = icon::sun;
    static constexpr bool property_grid = true;
    static void draw(entt::registry&, entt::entity, scene::DirectionalLight& light) {
        (void)ui::prop_vec3("Direction", light.direction,
                            {.speed = 0.01F, .min = -1.0F, .max = 1.0F},
                            {.tooltip = "Travel direction of the light (sun to scene)"});
        (void)ui::prop_color3("Colour", light.color, {.reset = glm::vec3(1.0F)});
        (void)ui::prop_float("Intensity", light.intensity,
                             {.speed = 0.05F, .min = 0.0F, .max = 100.0F}, {.reset = 1.0F});
    }
};

void register_dir_light_inspector(InspectorRegistry& registry) {
    registry.add({.name = "dir_light",
                  .order = 500,
                  .draw = [](panels::InspectorState& state, entt::registry& r, entt::entity e) {
            inspect<scene::DirectionalLight>(state, r, e, true);
        }});
}

} // namespace engine::editor
