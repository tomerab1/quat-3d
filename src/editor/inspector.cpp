#include "inspector.hpp"

#define GLM_ENABLE_EXPERIMENTAL
#include <algorithm>
#include <cstring>
#include <string>
#include <utility>

#include <glm/glm.hpp>
#include <glm/gtc/quaternion.hpp>
#include <glm/gtx/matrix_decompose.hpp>
#include <imgui.h>
#include <imgui_stdlib.h>

#include "animation_inspector.hpp"
#include "audio_inspector.hpp"
#include "behavior_panel.hpp"
#include "grass_inspector.hpp"
#include "particle_inspector.hpp"
#include "script_inspector.hpp"
#include "tuning_panels.hpp"
#include "engine/asset/material_asset.hpp"
#include "engine/asset/mesh_asset.hpp"
#include "engine/editor/editor.hpp"
#include "engine/scene/behavior.hpp"
#include "engine/scene/components.hpp"
#include "engine/scene/scatter.hpp"
#include "engine/scene/scene.hpp"
#include "inspector_world.hpp"
#include "panels/inspector_parts.hpp"
#include "shell/panels.hpp"
#include "ui/fonts.hpp"
#include "ui/icons.hpp"
#include "ui/metrics.hpp"
#include "ui/widgets.hpp"

namespace engine::editor {

namespace {

// One editor per component type; the generic `inspect<T>` below finds the
// component on the entity and draws its section header (ui::component_header)
// and, for editors declaring `property_grid`, the label / value grid.
// Optional members: `codec` (scene-codec name: enables reset / copy / paste),
// `icon`, `enabled(T&)` (an enable toggle in the header).
template <typename T>
struct ComponentInspector;

// Small read-only / explanatory line under a component's fields.
void hint(const char* text) {
    ImGui::PushTextWrapPos(0.0F);
    ui::text_colored(ui::tokens().text_secondary, text);
    ImGui::PopTextWrapPos();
}

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

template <>
struct ComponentInspector<scene::Terrain> {
    static constexpr const char* title = "Terrain";
    static constexpr const char* codec = "terrain";
    static constexpr const char* icon = icon::mountain;
    static constexpr bool property_grid = true;
    static void draw(entt::registry&, entt::entity, scene::Terrain& terrain) {
        draw_terrain(terrain);
    }
};

template <>
struct ComponentInspector<scene::Grass> {
    static constexpr const char* title = "Grass";
    static constexpr const char* codec = "grass";
    static constexpr const char* icon = icon::sprout;
    static void draw(entt::registry&, entt::entity, scene::Grass& grass) { draw_grass(grass); }
};

// GPU particles (R10.3): the emitter's own editor (plain ImGui widgets, no
// property grid).
template <>
struct ComponentInspector<scene::ParticleEmitter> {
    static constexpr const char* title = "Particle Emitter";
    static constexpr const char* codec = "particle_emitter";
    static constexpr const char* icon = icon::sparkles;
    static void draw(entt::registry&, entt::entity, scene::ParticleEmitter& emitter) {
        draw_particle_emitter(emitter);
    }
};

// Gameplay script (G7): file, enable, status and the file's declared
// properties (read from the script runtime).
template <>
struct ComponentInspector<scene::Script> {
    static constexpr const char* title = "Script";
    static constexpr const char* codec = "script";
    static constexpr const char* icon = icon::file_code;
    static void draw(entt::registry&, entt::entity, scene::Script& script,
                     const panels::InspectorState& state) {
        draw_script_fields(script, state.scripts);
    }
};

template <>
struct ComponentInspector<scene::Primitive> {
    static constexpr const char* title = "Primitive";
    static constexpr const char* codec = "primitive";
    static constexpr const char* icon = icon::shapes;
    static constexpr bool property_grid = true;
    static void draw(entt::registry&, entt::entity, scene::Primitive& p) {
        draw_primitive(p);
    }
};

template <>
struct ComponentInspector<scene::InstancedMesh> {
    static constexpr const char* title = "Instanced Mesh";
    static constexpr const char* codec = "instanced_mesh";
    static constexpr const char* icon = icon::boxes;
    static constexpr bool property_grid = true;
    static void draw(entt::registry& r, entt::entity e, scene::InstancedMesh& mesh,
                     const panels::InspectorState& state) {
        draw_instanced_mesh(r, e, mesh, state.project_root);
    }
};

template <>
struct ComponentInspector<scene::Scatter> {
    static constexpr const char* title = "Scatter";
    static constexpr const char* codec = "scatter";
    static constexpr const char* icon = icon::trees;
    static constexpr bool property_grid = true;
    static void draw(entt::registry& r, entt::entity e, scene::Scatter& s) {
        draw_scatter(r, e, s);
    }
};

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

template <>
struct ComponentInspector<scene::BehaviorTree> {
    static constexpr const char* title = "Behavior Tree";
    static constexpr const char* codec = "behavior";
    static constexpr const char* icon = icon::workflow;
    static bool* enabled(scene::BehaviorTree& bt) { return &bt.enabled; }
    static void draw(entt::registry&, entt::entity, scene::BehaviorTree& bt) {
        if (ui::button(ui::label(icon::workflow, "Edit graph").c_str(), ui::ButtonKind::primary,
                       ImVec2(-1.0F, 0.0F))) {
            ImGui::SetWindowFocus(behavior_panel_title);
        }
        // Resizable: trees with saved editor layout outgrow any fixed buffer.
        {
            ui::FontScope mono(ui::FontRole::mono);
            ImGui::InputTextMultiline("##bt_json", &bt.source,
                                      ImVec2(-1.0F, ImGui::GetTextLineHeight() * 8.0F));
        }
        if (ui::button(ui::label(icon::refresh_cw, "Recompile").c_str(), ui::ButtonKind::secondary,
                       ImVec2(-1.0F, 0.0F))) {
            bt.compiled.reset();
            bt.parse_failed = false;
        }
        if (bt.parse_failed) {
            ui::text_colored(ui::tokens().error, ui::label(icon::circle_x, "Parse failed (see the Console)"));
        } else if (bt.compiled != nullptr) {
            ui::text_colored(ui::tokens().success,
                             ui::label(icon::circle_check, "Compiled: " +
                                                               std::to_string(bt.compiled->nodes().size()) +
                                                               " nodes"));
        }
        std::string leaves;
        for (const scene::BtLeafInfo& leaf : scene::bt_builtin_leaf_info()) {
            leaves += (leaves.empty() ? "" : ", ") + std::string(leaf.name);
        }
        const std::string text = "JSON: sequence/selector (resume the running child), "
                                 "reactive_sequence/reactive_selector (re-check every tick, "
                                 "pre-empting), inverter; leaves " + leaves + ".";
        hint(text.c_str());
    }
};

template <>
struct ComponentInspector<scene::Camera> {
    static constexpr const char* title = "Camera";
    static constexpr const char* codec = "camera";
    static constexpr const char* icon = icon::camera;
    static constexpr bool property_grid = true;
    static void draw(entt::registry& r, entt::entity e, scene::Camera& camera) {
        (void)ui::prop_angle("Field of view", camera.fov_y,
                             {.min = 10.0F, .max = 140.0F, .format = "%.0f°", .slider = true},
                             {.tooltip = "Vertical field of view"});
        (void)ui::prop_float("Near", camera.near_z, {.speed = 0.01F, .min = 0.001F, .max = 100.0F,
                                                     .format = "%.3f m"});
        (void)ui::prop_float("Far", camera.far_z, {.speed = 1.0F, .min = 0.1F, .max = 100000.0F,
                                                   .format = "%.0f m"});
        // Exactly one camera owns the play view: activating this one
        // deactivates the rest (radio semantics, like a "main camera" tag).
        if (ui::prop_bool("Active", camera.is_active,
                          {.tooltip = "Play + Game camera views through the active camera"}) &&
            camera.is_active) {
            for (auto [other, cam] : r.view<scene::Camera>().each()) {
                if (other != e) cam.is_active = false;
            }
        }
        ui::prop_note("Put the camera on a child entity offset from the body; on the mesh itself "
                      "the view sits inside the geometry.");
    }
};

template <>
struct ComponentInspector<scene::MeshRenderer> {
    static constexpr const char* title = "Mesh Renderer";
    static constexpr const char* icon = icon::box;
    static constexpr bool property_grid = true;
    static void draw(entt::registry&, entt::entity, scene::MeshRenderer& mr) {
        char buf[128];
        if (mr.mesh.valid() && mr.mesh.is_loaded()) {
            std::snprintf(buf, sizeof(buf), "%u vertices, %u indices", mr.mesh->vertex_count,
                          mr.mesh->index_count);
            ui::prop_value("Mesh", buf, nullptr, true);
        } else {
            ui::prop_value("Mesh", "none");
        }
        if (mr.material.valid() && mr.material.is_loaded()) {
            const auto& p = mr.material->params;
            std::snprintf(buf, sizeof(buf), "%.2f %.2f %.2f", static_cast<double>(p.base_color_factor.x),
                          static_cast<double>(p.base_color_factor.y),
                          static_cast<double>(p.base_color_factor.z));
            ui::prop_value("Base colour", buf, nullptr, true);
            std::snprintf(buf, sizeof(buf), "metal %.2f  rough %.2f",
                          static_cast<double>(p.metallic_factor),
                          static_cast<double>(p.roughness_factor));
            ui::prop_value("Surface", buf, nullptr, true);
            std::snprintf(buf, sizeof(buf), "0x%x", p.flags);
            ui::prop_value("Flags", buf, "Material feature flags", true);
        } else {
            ui::prop_value("Material", "default");
        }
    }
};

template <>
struct ComponentInspector<scene::Animator> {
    static constexpr const char* title = "Animator";
    static constexpr const char* codec = "animator";
    static constexpr const char* icon = icon::clapperboard;
    static constexpr bool property_grid = true;
    static void draw(entt::registry&, entt::entity, scene::Animator& animator) {
        draw_animator(animator);
    }
};

template <>
struct ComponentInspector<scene::FootIK> {
    static constexpr const char* title = "Foot IK";
    static constexpr const char* codec = "foot_ik";
    static constexpr const char* icon = icon::footprints;
    static constexpr bool property_grid = true;
    static bool* enabled(scene::FootIK& k) { return &k.enabled; }
    static void draw(entt::registry& r, entt::entity e, scene::FootIK& foot_ik) {
        draw_foot_ik(r, e, foot_ik);
    }
};

template <>
struct ComponentInspector<scene::Collider> {
    static constexpr const char* title = "Collider";
    static constexpr const char* codec = "collider";
    static constexpr const char* icon = icon::shield;
    static constexpr bool property_grid = true;
    static void draw(entt::registry&, entt::entity, scene::Collider& collider) {
        static constexpr ui::EnumItem shapes[] = {
            {static_cast<int>(scene::ColliderShape::box), "Box", nullptr},
            {static_cast<int>(scene::ColliderShape::sphere), "Sphere", nullptr},
            {static_cast<int>(scene::ColliderShape::capsule), "Capsule", nullptr},
        };
        (void)ui::prop_enum("Shape", collider.shape, shapes);
        const ui::DragSpec size{.speed = 0.05F, .min = 0.01F, .max = 1000.0F};
        switch (collider.shape) {
        case scene::ColliderShape::box:
            (void)ui::prop_vec3("Half extents", collider.half_extents, size);
            break;
        case scene::ColliderShape::sphere:
            (void)ui::prop_float("Radius", collider.half_extents.x, size);
            break;
        case scene::ColliderShape::capsule:
            (void)ui::prop_float("Radius", collider.half_extents.x, size);
            (void)ui::prop_float("Half height", collider.half_extents.y, size);
            break;
        }
        (void)ui::prop_vec3("Offset", collider.offset, {.speed = 0.05F}, {.reset = glm::vec3(0.0F)});
        (void)ui::prop_bool("Trigger", collider.is_sensor,
                            {.reset = false, .tooltip = "Overlap events only: objects pass through"});
        if (collider.is_sensor) {
            ui::prop_note("No collision response: objects pass through.", ui::tokens().warning);
        }
        ui::prop_note("Without a Rigid Body this simulates as static; add a dynamic Rigid Body "
                      "to make it fall in Play.");
    }
};

template <>
struct ComponentInspector<scene::RigidBody> {
    static constexpr const char* title = "Rigid Body";
    static constexpr const char* codec = "rigidbody";
    static constexpr const char* icon = icon::atom;
    static constexpr bool property_grid = true;
    static void draw(entt::registry&, entt::entity, scene::RigidBody& body) {
        static constexpr ui::EnumItem motions[] = {
            {static_cast<int>(scene::BodyMotion::static_body), "Static", "Never moves"},
            {static_cast<int>(scene::BodyMotion::kinematic), "Kinematic", "Moved by the transform"},
            {static_cast<int>(scene::BodyMotion::dynamic), "Dynamic", "Simulated by Jolt"},
        };
        (void)ui::prop_enum("Motion", body.motion, motions);
        (void)ui::prop_float("Mass", body.mass,
                             {.speed = 0.1F, .min = 0.01F, .max = 10000.0F, .format = "%.2f kg"});
        ui::prop_value("Body", body.body == 0xFFFFFFFFU ? "not created (press Play)" : "live");
    }
};

template <>
struct ComponentInspector<scene::AudioSource> {
    static constexpr const char* title = "Audio Source";
    static constexpr const char* codec = "audio_source";
    static constexpr const char* icon = icon::volume_2;
    static void draw(entt::registry&, entt::entity, scene::AudioSource& source) {
        draw_audio_source(source);
    }
};

template <>
struct ComponentInspector<scene::AudioListener> {
    static constexpr const char* title = "Audio Listener";
    static constexpr const char* codec = "audio_listener";
    static constexpr const char* icon = icon::ear;
    static void draw(entt::registry& r, entt::entity e, scene::AudioListener& listener) {
        draw_audio_listener(r, e, listener);
    }
};

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

} // namespace

void draw_inspector(scene::Scene& scene, entt::entity selected, panels::InspectorState& state,
                    bool* open) {
    if (open != nullptr && !*open) return;
    if (!ImGui::Begin(shell::panel_window_name(shell::Panel::inspector).c_str(), open)) {
        ImGui::End();
        return;
    }
    entt::registry& r = scene.registry();
    if (selected == entt::null || !r.valid(selected)) {
        (void)ui::empty_state(icon::mouse_pointer_2, "Nothing selected",
                              "Select an entity in the Hierarchy or click it in the viewport.");
        ImGui::End();
        return;
    }

    panels::entity_header(scene, selected);
    inspect<scene::Transform>(state, r, selected);
    inspect<scene::Primitive>(state, r, selected, true);
    if (!r.all_of<scene::Primitive>(selected)) inspect<scene::MeshRenderer>(state, r, selected);
    draw_material_inspector(r, selected);
    inspect<scene::DirectionalLight>(state, r, selected, true);
    inspect<scene::PointLight>(state, r, selected, true);
    inspect<scene::Camera>(state, r, selected, true);
    inspect<scene::Animator>(state, r, selected);
    inspect<scene::FootIK>(state, r, selected, true);
    inspect<scene::Terrain>(state, r, selected, true);
    inspect<scene::Grass>(state, r, selected, true);
    inspect<scene::ParticleEmitter>(state, r, selected, true);
    inspect<scene::InstancedMesh>(state, r, selected, true); // removal takes parts + scatter
    inspect<scene::Scatter>(state, r, selected, true);
    inspect<scene::NavAgent>(state, r, selected, true);
    inspect<scene::BehaviorTree>(state, r, selected, true);
    inspect<scene::PatrolRoute>(state, r, selected, true);
    inspect<scene::Script>(state, r, selected, true);
    inspect<scene::Collider>(state, r, selected, true);
    inspect<scene::RigidBody>(state, r, selected, true);
    inspect<scene::AudioSource>(state, r, selected, true);
    inspect<scene::AudioListener>(state, r, selected, true);

    panels::add_component_button(r, selected, state);
    ImGui::End();
}

} // namespace engine::editor
