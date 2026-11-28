// Inspector section: Transform (ROADMAP S9: one file per component).
// Registered by the generated inspectors.cpp; see inspectors/inspect.hpp.

#define GLM_ENABLE_EXPERIMENTAL
#include <glm/gtc/quaternion.hpp>
#include <glm/gtx/matrix_decompose.hpp>

#include "inspect.hpp"
#include "../time_of_day_panel.hpp"

namespace engine::editor {

template <>
struct ComponentInspector<scene::Transform> {
    static constexpr const char* title = "Transform";
    static constexpr const char* codec = "transform";
    static constexpr const char* icon = icon::move;
    static constexpr bool property_grid = true;
    static void draw(entt::registry&, entt::entity, scene::Transform& transform) {
        glm::vec3 translation{};
        glm::vec3 scale{};
        glm::quat rotation{};
        glm::vec3 skew{};
        glm::vec4 perspective{};
        if (!glm::decompose(transform.local, scale, rotation, translation, skew, perspective)) {
            ui::prop_value("Matrix", "not decomposable (shear / projection)");
            return;
        }
        glm::vec3 euler_deg = glm::degrees(glm::eulerAngles(rotation));

        bool changed = false;
        changed |= ui::prop_vec3("Position", translation, {.speed = 0.05F},
                                 {.reset = glm::vec3(0.0F)});
        changed |= ui::prop_vec3("Rotation", euler_deg, {.speed = 0.5F, .format = "%.1f°"},
                                 {.reset = glm::vec3(0.0F), .tooltip = "Euler angles, degrees"});
        changed |= ui::prop_vec3("Scale", scale, {.speed = 0.02F, .min = 0.001F, .max = 1000.0F},
                                 {.reset = glm::vec3(1.0F)});
        if (changed) {
            transform.local = glm::translate(glm::mat4(1.0F), translation) *
                              glm::mat4_cast(glm::quat(glm::radians(euler_deg))) *
                              glm::scale(glm::mat4(1.0F), scale);
        }
    }
};

void register_transform_inspector(InspectorRegistry& registry) {
    registry.add({.name = "transform",
                  .order = 100,
                  .draw = [](panels::InspectorState& state, entt::registry& r, entt::entity e) {
            if (clock_only_entity(r, e)) return; // a clock-only entity has no place in the world
            inspect<scene::Transform>(state, r, e);
        }});
}

} // namespace engine::editor
