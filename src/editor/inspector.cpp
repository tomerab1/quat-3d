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
#include "tuning_panels.hpp"
#include "grass_inspector.hpp"
#include "engine/editor/editor.hpp"
#include "engine/scene/behavior.hpp"
#include "engine/scene/components.hpp"
#include "engine/scene/scatter.hpp"
#include "engine/scene/scene.hpp"

namespace engine::editor {

namespace {

// One editor per component type; the generic `inspect<T>` below finds the
// component on the entity and wraps the editor in a collapsing header.
template <typename T>
struct ComponentInspector;

template <>
struct ComponentInspector<scene::Name> {
    static constexpr const char* title = "Name";
    static void draw(entt::registry&, entt::entity, scene::Name& name) {
        char buffer[128]{};
        std::strncpy(buffer, name.value.c_str(), sizeof(buffer) - 1);
        if (ImGui::InputText("name", buffer, sizeof(buffer))) {
            name.value = buffer;
        }
    }
};

template <>
struct ComponentInspector<scene::Transform> {
    static constexpr const char* title = "Transform";
    static void draw(entt::registry&, entt::entity, scene::Transform& transform) {
        glm::vec3 translation{};
        glm::vec3 scale{};
        glm::quat rotation{};
        glm::vec3 skew{};
        glm::vec4 perspective{};
        if (!glm::decompose(transform.local, scale, rotation, translation, skew, perspective)) {
            ImGui::TextUnformatted("(non-decomposable matrix)");
            return;
        }
        glm::vec3 euler_deg = glm::degrees(glm::eulerAngles(rotation));

        bool changed = false;
        changed |= ImGui::DragFloat3("translation", &translation.x, 0.05F);
        changed |= ImGui::DragFloat3("rotation (deg)", &euler_deg.x, 0.5F);
        changed |= ImGui::DragFloat3("scale", &scale.x, 0.02F, 0.001F, 1000.0F);
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
    static void draw(entt::registry&, entt::entity, scene::DirectionalLight& light) {
        ImGui::DragFloat3("direction", &light.direction.x, 0.01F, -1.0F, 1.0F);
        ImGui::ColorEdit3("color", &light.color.x);
        ImGui::DragFloat("intensity", &light.intensity, 0.05F, 0.0F, 100.0F);
    }
};

template <>
struct ComponentInspector<scene::PointLight> {
    static constexpr const char* title = "Point Light";
    static void draw(entt::registry&, entt::entity, scene::PointLight& light) {
        ImGui::ColorEdit3("color", &light.color.x);
        ImGui::DragFloat("radius", &light.radius, 0.1F, 0.01F, 1000.0F);
        ImGui::DragFloat("intensity", &light.intensity, 0.05F, 0.0F, 100.0F);
    }
};

template <>
struct ComponentInspector<scene::Terrain> {
    static constexpr const char* title = "Terrain";
    static void draw(entt::registry&, entt::entity, scene::Terrain& terrain) {
        terrain::TerrainParams& p = terrain.params;
        int seed = static_cast<int>(p.seed);
        if (ImGui::DragInt("seed", &seed, 1.0F, 0, 1 << 30)) {
            p.seed = static_cast<std::uint32_t>(seed);
        }
        ImGui::DragFloat("tile size", &p.tile_size_m, 10.0F, 100.0F, 20000.0F, "%.0f m");
        ImGui::DragFloat("height", &p.height_m, 1.0F, 1.0F, 2000.0F, "%.0f m");
        ImGui::SliderInt("octaves", &p.octaves, 1, 10);
        ImGui::SliderFloat("warp", &p.warp_strength, 0.0F, 1.0F, "%.2f");
        int model = p.erosion_model == terrain::ErosionModel::grid ? 1 : 0;
        if (ImGui::Combo("erosion", &model, "droplets (CPU)\0grid (GPU)\0")) {
            p.erosion_model =
                model == 1 ? terrain::ErosionModel::grid : terrain::ErosionModel::droplets;
        }
        if (p.erosion_model == terrain::ErosionModel::grid) {
            int steps = static_cast<int>(p.grid.iterations);
            if (ImGui::DragInt("erosion steps", &steps, 5.0F, 0, 5'000)) {
                p.grid.iterations = static_cast<std::uint32_t>(glm::max(steps, 0));
            }
        } else {
            int droplets = static_cast<int>(p.erosion_droplets);
            if (ImGui::DragInt("erosion droplets", &droplets, 1000.0F, 0, 1'000'000)) {
                p.erosion_droplets = static_cast<std::uint32_t>(glm::max(droplets, 0));
            }
        }
        // CPU and GPU synthesis produce the same heights: the choice applies
        // to the next tile generated (no regeneration).
        ImGui::Checkbox("generate on GPU", &terrain.gpu_generate);
        ImGui::DragFloat("snowline", &terrain.snowline_m, 1.0F, -1000.0F, 5000.0F, "%.0f m");
        ImGui::Separator();
        if (ImGui::Checkbox("streaming", &terrain.streaming)) {
            terrain.regenerate = true; // rebuild in the new mode
        }
        if (terrain.streaming) {
            // The runtime resizes the window and keeps the loaded tiles.
            ImGui::SliderInt("radius (tiles)", &terrain.stream_radius, 1, 4);
            ImGui::TextWrapped("An endless seeded world streams in around the camera.");
        }
        if (ImGui::Button("Regenerate", ImVec2(-1.0F, 0.0F))) {
            terrain.regenerate = true;
        }
        ImGui::TextWrapped("%s", terrain.gpu_generate
                                     ? "Tiles synthesise on the GPU (droplet erosion stays on a "
                                       "CPU worker); they pop in when ready."
                                     : "Generation runs on a worker thread; tiles pop in when "
                                       "ready.");
    }
};

template <>
struct ComponentInspector<scene::Grass> {
    static constexpr const char* title = "Grass";
    static void draw(entt::registry&, entt::entity, scene::Grass& grass) { draw_grass(grass); }
};

template <>
struct ComponentInspector<scene::InstancedMesh> {
    static constexpr const char* title = "Instanced Mesh";
    static void draw(entt::registry& r, entt::entity e, scene::InstancedMesh& mesh) {
        // Paths commit on Enter: every edit re-resolves the model.
        char path[512]{};
        std::strncpy(path, mesh.gltf_path.c_str(), sizeof(path) - 1);
        if (ImGui::InputText("glTF", path, sizeof(path), ImGuiInputTextFlags_EnterReturnsTrue)) {
            mesh.gltf_path = path;
        }
        if (ImGui::IsItemHovered()) {
            ImGui::SetTooltip("Model file relative to the project root (Enter to apply);\n"
                              "empty = the primitive shape below");
        }
        if (!mesh.gltf_path.empty()) {
            char node[256]{};
            std::strncpy(node, mesh.gltf_node.c_str(), sizeof(node) - 1);
            if (ImGui::InputText("node", node, sizeof(node), ImGuiInputTextFlags_EnterReturnsTrue)) {
                mesh.gltf_node = node;
            }
            if (ImGui::IsItemHovered()) ImGui::SetTooltip("Only this node's subtree (empty = all)");
        } else {
            scene::Primitive& p = mesh.primitive;
            const char* shapes[] = {"cube", "sphere", "plane", "cylinder", "cone", "capsule"};
            int shape = static_cast<int>(p.shape);
            if (ImGui::Combo("shape", &shape, shapes, 6)) {
                p.shape = static_cast<scene::PrimitiveShape>(shape);
            }
            ImGui::ColorEdit4("color", &p.color.x);
            ImGui::SliderFloat("metallic", &p.metallic, 0.0F, 1.0F);
            ImGui::SliderFloat("roughness", &p.roughness, 0.0F, 1.0F);
            ImGui::ColorEdit3("emissive", &p.emissive.x, ImGuiColorEditFlags_HDR);
        }
        ImGui::Checkbox("cast shadows", &mesh.cast_shadows);

        if (const auto* parts = r.try_get<scene::InstancedMeshParts>(e)) {
            ImGui::Text("model: %zu part(s), radius %.2f m", parts->parts.size(), parts->bounds.w);
        } else {
            ImGui::TextDisabled("model: resolving...");
        }
        ImGui::Separator();
        if (const auto* scatter = r.try_get<scene::Scatter>(e)) {
            ImGui::Text("%zu placements (scatter generation %u)", mesh.instances.size(),
                        scatter->generation);
            ImGui::TextDisabled("generated by the Scatter; remove it to edit by hand");
            return;
        }
        ImGui::Text("%zu placements", mesh.instances.size());
        constexpr int k_listed = 64; // a long list is edited through scene files / remote API
        int remove = -1;
        const int shown = std::min(static_cast<int>(mesh.instances.size()), k_listed);
        for (int i = 0; i < shown; ++i) {
            scene::InstanceTransform& t = mesh.instances[static_cast<std::size_t>(i)];
            ImGui::PushID(i);
            ImGui::DragFloat3("##pos", &t.position.x, 0.05F);
            ImGui::SameLine();
            if (ImGui::SmallButton("x")) remove = i;
            glm::vec3 euler = glm::degrees(glm::eulerAngles(t.rotation));
            if (ImGui::DragFloat3("##rot", &euler.x, 0.5F)) {
                t.rotation = glm::quat(glm::radians(euler));
            }
            float uniform = t.scale.x;
            if (ImGui::DragFloat("##scale", &uniform, 0.01F, 0.01F, 1000.0F, "scale %.2f")) {
                t.scale = glm::vec3(uniform);
            }
            ImGui::PopID();
        }
        if (static_cast<int>(mesh.instances.size()) > shown) {
            ImGui::TextDisabled("(%zu more not listed)", mesh.instances.size() - shown);
        }
        if (remove >= 0) mesh.instances.erase(mesh.instances.begin() + remove);
        if (ImGui::Button("Add placement")) {
            scene::InstanceTransform t;
            if (!mesh.instances.empty()) {
                t.position = mesh.instances.back().position + glm::vec3(2.0F, 0.0F, 0.0F);
            }
            mesh.instances.push_back(t);
        }
        ImGui::SameLine();
        if (!mesh.instances.empty() && ImGui::Button("Clear")) mesh.instances.clear();
    }
};

template <>
struct ComponentInspector<scene::Scatter> {
    static constexpr const char* title = "Scatter";
    static void draw(entt::registry& r, entt::entity e, scene::Scatter& s) {
        int seed = static_cast<int>(s.seed & 0x7FFFFFFFU);
        if (ImGui::DragInt("seed", &seed, 1.0F, 0, 0x7FFFFFFF)) {
            s.seed = static_cast<std::uint32_t>(seed);
        }
        const char* areas[] = {"rectangle", "circle"};
        int area = static_cast<int>(s.area);
        if (ImGui::Combo("area", &area, areas, 2)) s.area = static_cast<scene::ScatterArea>(area);
        if (s.area == scene::ScatterArea::rect) {
            ImGui::DragFloat2("size (x, z)", &s.size.x, 0.5F, 0.0F, 20000.0F, "%.1f m");
        } else {
            ImGui::DragFloat("radius", &s.radius, 0.25F, 0.0F, 10000.0F, "%.1f m");
        }
        ImGui::DragFloat("density", &s.density, 0.001F, 0.0F, 100.0F, "%.3f / m2");
        ImGui::DragFloat("min spacing", &s.min_spacing, 0.05F, 0.0F, 1000.0F, "%.2f m");
        const char* patterns[] = {"poisson disc", "jittered grid"};
        int pattern = static_cast<int>(s.pattern);
        if (ImGui::Combo("pattern", &pattern, patterns, 2)) {
            s.pattern = static_cast<scene::ScatterPattern>(pattern);
        }
        ImGui::DragFloatRange2("scale", &s.scale_range.x, &s.scale_range.y, 0.01F, 0.01F, 100.0F);
        ImGui::Checkbox("random yaw", &s.random_yaw);
        ImGui::SliderFloat("align to ground", &s.align_to_normal, 0.0F, 1.0F);
        ImGui::DragFloat("base offset", &s.base_offset, 0.01F, -100.0F, 100.0F, "%.2f x scale");
        if (ImGui::IsItemHovered()) {
            ImGui::SetTooltip("Lift along the placement's up axis, times its scale\n"
                              "(0.5 seats a unit primitive on the ground)");
        }
        ImGui::DragFloatRange2("slope", &s.slope_deg.x, &s.slope_deg.y, 0.5F, 0.0F, 90.0F,
                               "%.0f deg");
        ImGui::DragFloatRange2("height", &s.height_m.x, &s.height_m.y, 1.0F, -100000.0F,
                               100000.0F, "%.0f m");
        int max_instances = static_cast<int>(s.max_instances);
        if (ImGui::DragInt("max instances", &max_instances, 100.0F, 0,
                           static_cast<int>(scene::k_scatter_instance_limit))) {
            s.max_instances = static_cast<std::uint32_t>(std::max(max_instances, 0));
        }
        const auto* mesh = r.try_get<scene::InstancedMesh>(e);
        ImGui::Text("%zu placements (generation %u)", mesh != nullptr ? mesh->instances.size() : 0,
                    s.generation);
        ImGui::TextWrapped("Regenerates when these settings, the entity's position or the "
                           "terrain change. Removing the Scatter keeps the placements.");
    }
};

template <>
struct ComponentInspector<scene::NavAgent> {
    static constexpr const char* title = "Nav Agent";
    static void draw(entt::registry& r, entt::entity e, scene::NavAgent& agent) {
        // Editing the target mid-walk is an order: a patrol route waits for it.
        if (ImGui::DragFloat3("target", &agent.target.x, 0.1F) && agent.active) {
            agent.commanded = true;
        }
        ImGui::DragFloat("speed", &agent.speed, 0.1F, 0.1F, 50.0F, "%.1f m/s");
        ImGui::DragFloat("ground offset", &agent.ground_offset, 0.05F, 0.0F, 10.0F, "%.2f m");
        if (ImGui::IsItemHovered()) {
            ImGui::SetTooltip("Entity origin height above the ground while walking\n"
                              "(half-height for a centre-pivot mesh, e.g. 0.5 for the unit cube)");
        }
        const auto* route = r.try_get<scene::PatrolRoute>(e);
        const bool patrolling = route != nullptr && !route->points.empty();
        const bool running = !agent.paused && (agent.active || patrolling);
        if (ImGui::Button(running ? "Stop" : "Go", ImVec2(-1.0F, 0.0F))) {
            if (running) {
                scene::nav_agent_stop(agent);
            } else {
                scene::nav_agent_resume(r, e);
            }
        }
        ImGui::Text("%s", agent.paused   ? "stopped"
                          : agent.active ? "moving"
                          : agent.arrived ? "arrived"
                          : agent.failed ? "failed (unreachable or stuck)"
                                         : "idle");
        ImGui::TextWrapped("Bake a navmesh (Physics panel) and press Play; the agent "
                           "paths to the target.");
    }
};

template <>
struct ComponentInspector<scene::PatrolRoute> {
    static constexpr const char* title = "Patrol Route";
    static void draw(entt::registry&, entt::entity, scene::PatrolRoute& route) {
        ImGui::Checkbox("loop", &route.loop);
        int remove = -1;
        for (int i = 0; i < static_cast<int>(route.points.size()); ++i) {
            ImGui::PushID(i);
            ImGui::DragFloat3("##pt", &route.points[static_cast<std::size_t>(i)].x, 0.1F);
            ImGui::SameLine();
            if (ImGui::SmallButton("x")) remove = i;
            ImGui::PopID();
        }
        if (remove >= 0) {
            route.points.erase(route.points.begin() + remove);
            route.next = 0;
            route.finished = false;
        }
        if (!route.points.empty() && ImGui::Button("Clear", ImVec2(-1.0F, 0.0F))) {
            route.points.clear();
            route.next = 0;
            route.finished = false;
        }
        ImGui::TextWrapped("Shift+click the ground in the viewport to add points; "
                           "Ctrl+click sends the agent somewhere once.");
    }
};

template <>
struct ComponentInspector<scene::BehaviorTree> {
    static constexpr const char* title = "Behavior Tree";
    static void draw(entt::registry&, entt::entity, scene::BehaviorTree& bt) {
        ImGui::Checkbox("enabled", &bt.enabled);
        if (ImGui::Button("Edit graph", ImVec2(-1.0F, 0.0F))) {
            ImGui::SetWindowFocus(behavior_panel_title);
        }
        // Resizable: trees with saved editor layout outgrow any fixed buffer.
        ImGui::InputTextMultiline("##bt_json", &bt.source,
                                  ImVec2(-1.0F, ImGui::GetTextLineHeight() * 8.0F));
        if (ImGui::Button("Recompile", ImVec2(-1.0F, 0.0F))) {
            bt.compiled.reset();
            bt.parse_failed = false;
        }
        if (bt.parse_failed) {
            ImGui::TextColored(ImVec4(1.0F, 0.4F, 0.3F, 1.0F), "parse failed (see log)");
        } else if (bt.compiled != nullptr) {
            ImGui::Text("compiled: %zu nodes", bt.compiled->nodes().size());
        }
        std::string leaves;
        for (const scene::BtLeafInfo& leaf : scene::bt_builtin_leaf_info()) {
            leaves += (leaves.empty() ? "" : ", ") + std::string(leaf.name);
        }
        ImGui::TextWrapped("JSON: sequence/selector (resume the running child), "
                           "reactive_sequence/reactive_selector (re-check every tick, "
                           "pre-empting), inverter; leaves %s.",
                           leaves.c_str());
    }
};

template <>
struct ComponentInspector<scene::Camera> {
    static constexpr const char* title = "Camera";
    static void draw(entt::registry& r, entt::entity e, scene::Camera& camera) {
        float fov_deg = glm::degrees(camera.fov_y);
        if (ImGui::SliderFloat("fov (deg)", &fov_deg, 10.0F, 140.0F)) {
            camera.fov_y = glm::radians(fov_deg);
        }
        ImGui::DragFloat("near", &camera.near_z, 0.01F, 0.001F, 100.0F);
        ImGui::DragFloat("far", &camera.far_z, 1.0F, 0.1F, 100000.0F);
        // Exactly one camera owns the play view: activating this one
        // deactivates the rest (radio semantics, like a "main camera" tag).
        if (ImGui::Checkbox("active", &camera.is_active) && camera.is_active) {
            for (auto [other, cam] : r.view<scene::Camera>().each()) {
                if (other != e) cam.is_active = false;
            }
        }
        ImGui::TextWrapped("Play + \"Game camera\" (menu bar) views through the active "
                           "camera. Put it on a child entity offset from the body — on the "
                           "mesh itself the view sits inside the geometry.");
    }
};

template <>
struct ComponentInspector<scene::MeshRenderer> {
    static constexpr const char* title = "Mesh Renderer";
    static void draw(entt::registry&, entt::entity, scene::MeshRenderer& mr) {
        if (mr.mesh.valid() && mr.mesh.is_loaded()) {
            ImGui::Text("mesh: %u vertices, %u indices", mr.mesh->vertex_count,
                        mr.mesh->index_count);
        } else {
            ImGui::TextUnformatted("mesh: <none>");
        }
        if (mr.material.valid() && mr.material.is_loaded()) {
            const auto& p = mr.material->params;
            ImGui::Text("material: flags 0x%x", p.flags);
            ImGui::Text("  base %.2f %.2f %.2f | metal %.2f rough %.2f", p.base_color_factor.x,
                        p.base_color_factor.y, p.base_color_factor.z, p.metallic_factor,
                        p.roughness_factor);
        } else {
            ImGui::TextUnformatted("material: <default>");
        }
    }
};

template <>
struct ComponentInspector<scene::Animator> {
    static constexpr const char* title = "Animator";
    static void draw(entt::registry&, entt::entity, scene::Animator& animator) {
        draw_animator(animator);
    }
};

template <>
struct ComponentInspector<scene::FootIK> {
    static constexpr const char* title = "Foot IK";
    static void draw(entt::registry& r, entt::entity e, scene::FootIK& foot_ik) {
        draw_foot_ik(r, e, foot_ik);
    }
};

template <>
struct ComponentInspector<scene::Collider> {
    static constexpr const char* title = "Collider";
    static void draw(entt::registry&, entt::entity, scene::Collider& collider) {
        const char* shapes[] = {"box", "sphere", "capsule"};
        int shape = static_cast<int>(collider.shape);
        if (ImGui::Combo("shape", &shape, shapes, 3)) {
            collider.shape = static_cast<scene::ColliderShape>(shape);
        }
        switch (collider.shape) {
        case scene::ColliderShape::box:
            ImGui::DragFloat3("half extents", &collider.half_extents.x, 0.05F, 0.01F, 1000.0F);
            break;
        case scene::ColliderShape::sphere:
            ImGui::DragFloat("radius", &collider.half_extents.x, 0.05F, 0.01F, 1000.0F);
            break;
        case scene::ColliderShape::capsule:
            ImGui::DragFloat("radius", &collider.half_extents.x, 0.05F, 0.01F, 1000.0F);
            ImGui::DragFloat("half height", &collider.half_extents.y, 0.05F, 0.01F, 1000.0F);
            break;
        }
        ImGui::DragFloat3("offset", &collider.offset.x, 0.05F);
        ImGui::Checkbox("trigger (overlap events only)", &collider.is_sensor);
        if (collider.is_sensor) {
            ImGui::TextColored(ImVec4(1.0F, 0.65F, 0.2F, 1.0F),
                               "no collision response: objects pass through");
        }
        ImGui::TextDisabled("without a Rigid Body this simulates as static;");
        ImGui::TextDisabled("add a dynamic Rigid Body to make it fall in Play");
    }
};

template <>
struct ComponentInspector<scene::RigidBody> {
    static constexpr const char* title = "Rigid Body";
    static void draw(entt::registry&, entt::entity, scene::RigidBody& body) {
        const char* motions[] = {"static", "kinematic", "dynamic"};
        int motion = static_cast<int>(body.motion);
        if (ImGui::Combo("motion", &motion, motions, 3)) {
            body.motion = static_cast<scene::BodyMotion>(motion);
        }
        ImGui::DragFloat("mass", &body.mass, 0.1F, 0.01F, 10000.0F);
        ImGui::TextDisabled(body.body == 0xFFFFFFFFU ? "body: not created (press Play)"
                                                     : "body: live");
    }
};

template <>
struct ComponentInspector<scene::AudioSource> {
    static constexpr const char* title = "Audio Source";
    static void draw(entt::registry&, entt::entity, scene::AudioSource& source) {
        draw_audio_source(source);
    }
};

template <>
struct ComponentInspector<scene::AudioListener> {
    static constexpr const char* title = "Audio Listener";
    static void draw(entt::registry& r, entt::entity e, scene::AudioListener& listener) {
        draw_audio_listener(r, e, listener);
    }
};

// Draws the component's collapsing header + editor. `removable` adds a
// right-click "Remove component" to the header; returns false if removed.
template <typename T>
bool inspect(entt::registry& registry, entt::entity entity, bool removable = false) {
    auto* component = registry.try_get<T>(entity);
    if (component == nullptr) return true;

    const bool open =
        ImGui::CollapsingHeader(ComponentInspector<T>::title, ImGuiTreeNodeFlags_DefaultOpen);
    if (removable && ImGui::BeginPopupContextItem(ComponentInspector<T>::title)) {
        const bool remove = ImGui::MenuItem("Remove component");
        ImGui::EndPopup();
        if (remove) {
            registry.remove<T>(entity);
            return false;
        }
    }
    if (open) {
        ImGui::PushID(ComponentInspector<T>::title);
        ComponentInspector<T>::draw(registry, entity, *component);
        ImGui::PopID();
    }
    return true;
}

// "Add Component" popup entry: shown only when the entity lacks T.
template <typename T, typename... Args>
void add_component_item(entt::registry& registry, entt::entity entity, Args&&... defaults) {
    if (registry.all_of<T>(entity)) return;
    if (ImGui::MenuItem(ComponentInspector<T>::title)) {
        registry.emplace<T>(entity, std::forward<Args>(defaults)...);
    }
}

} // namespace

void draw_inspector(scene::Scene& scene, entt::entity selected) {
    if (!ImGui::Begin("Inspector")) {
        ImGui::End();
        return;
    }
    entt::registry& r = scene.registry();
    if (selected == entt::null || !r.valid(selected)) {
        ImGui::TextUnformatted("(no entity selected)");
        ImGui::End();
        return;
    }

    ImGui::Text("entity %u", static_cast<std::uint32_t>(selected));
    ImGui::Separator();
    inspect<scene::Name>(r, selected);
    inspect<scene::Transform>(r, selected);
    inspect<scene::MeshRenderer>(r, selected);
    draw_material_inspector(r, selected);
    inspect<scene::DirectionalLight>(r, selected, true);
    inspect<scene::PointLight>(r, selected, true);
    inspect<scene::Camera>(r, selected, true);
    inspect<scene::Animator>(r, selected);
    inspect<scene::FootIK>(r, selected, true);
    inspect<scene::Terrain>(r, selected, true);
    inspect<scene::Grass>(r, selected, true);
    if (!inspect<scene::InstancedMesh>(r, selected, true)) {
        r.remove<scene::InstancedMeshParts, scene::Scatter>(selected); // as remove_component
    }
    inspect<scene::Scatter>(r, selected, true);
    inspect<scene::NavAgent>(r, selected, true);
    inspect<scene::BehaviorTree>(r, selected, true);
    inspect<scene::PatrolRoute>(r, selected, true);
    inspect<scene::Collider>(r, selected, true);
    inspect<scene::RigidBody>(r, selected, true);
    inspect<scene::AudioSource>(r, selected, true);
    inspect<scene::AudioListener>(r, selected, true);

    ImGui::Separator();
    if (ImGui::Button("Add component", ImVec2(-1.0F, 0.0F))) {
        ImGui::OpenPopup("add_component");
    }
    if (ImGui::BeginPopup("add_component")) {
        add_component_item<scene::Collider>(r, selected);
        add_component_item<scene::RigidBody>(r, selected);
        add_component_item<scene::Terrain>(r, selected);
        add_component_item<scene::Grass>(r, selected);
        add_component_item<scene::InstancedMesh>(r, selected);
        add_component_item<scene::Scatter>(r, selected); // adds an Instanced Mesh on demand
        add_component_item<scene::NavAgent>(r, selected);
        add_component_item<scene::BehaviorTree>(r, selected);
        add_component_item<scene::PatrolRoute>(r, selected);
        add_component_item<scene::PointLight>(r, selected, glm::vec3(1.0F), 10.0F, 5.0F);
        add_component_item<scene::DirectionalLight>(
            r, selected, glm::vec3(-0.4F, -1.0F, -0.3F), glm::vec3(1.0F), 3.0F);
        add_component_item<scene::Camera>(r, selected);
        add_component_item<scene::AudioSource>(r, selected);
        add_component_item<scene::AudioListener>(r, selected);
        if (r.all_of<scene::SkinnedMesh>(selected)) add_component_item<scene::FootIK>(r, selected);
        ImGui::EndPopup();
    }
    ImGui::End();
}

} // namespace engine::editor
