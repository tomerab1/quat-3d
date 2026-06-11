#include "inspector.hpp"

#define GLM_ENABLE_EXPERIMENTAL
#include <cstring>
#include <string>
#include <utility>

#include <glm/glm.hpp>
#include <glm/gtc/quaternion.hpp>
#include <glm/gtx/matrix_decompose.hpp>
#include <imgui.h>

#include "engine/editor/editor.hpp"
#include "engine/scene/behavior.hpp"
#include "engine/scene/components.hpp"
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
        int droplets = static_cast<int>(p.erosion_droplets);
        if (ImGui::DragInt("erosion droplets", &droplets, 1000.0F, 0, 1'000'000)) {
            p.erosion_droplets = static_cast<std::uint32_t>(glm::max(droplets, 0));
        }
        ImGui::DragFloat("snowline", &terrain.snowline_m, 1.0F, -1000.0F, 5000.0F, "%.0f m");
        ImGui::Separator();
        if (ImGui::Checkbox("streaming", &terrain.streaming)) {
            terrain.regenerate = true; // rebuild in the new mode
        }
        if (terrain.streaming) {
            if (ImGui::SliderInt("radius (tiles)", &terrain.stream_radius, 1, 4)) {
                terrain.regenerate = true;
            }
            ImGui::TextWrapped("An endless seeded world streams in around the camera.");
        }
        if (ImGui::Button("Regenerate", ImVec2(-1.0F, 0.0F))) {
            terrain.regenerate = true;
        }
        ImGui::TextWrapped("Generation runs on a worker thread; tiles pop in when ready.");
    }
};

template <>
struct ComponentInspector<scene::NavAgent> {
    static constexpr const char* title = "Nav Agent";
    static void draw(entt::registry&, entt::entity, scene::NavAgent& agent) {
        ImGui::DragFloat3("target", &agent.target.x, 0.1F);
        ImGui::DragFloat("speed", &agent.speed, 0.1F, 0.1F, 50.0F, "%.1f m/s");
        if (ImGui::Button(agent.active ? "Stop" : "Go", ImVec2(-1.0F, 0.0F))) {
            agent.active = !agent.active;
            agent.path.clear();
        }
        ImGui::Text(agent.arrived ? "arrived" : (agent.active ? "moving" : "idle"));
        ImGui::TextWrapped("Bake a navmesh (Physics panel) and press Play; the agent "
                           "paths to the target.");
    }
};

template <>
struct ComponentInspector<scene::BehaviorTree> {
    static constexpr const char* title = "Behavior Tree";
    static void draw(entt::registry&, entt::entity, scene::BehaviorTree& bt) {
        ImGui::Checkbox("enabled", &bt.enabled);
        static char buffer[4096];
        std::strncpy(buffer, bt.source.c_str(), sizeof(buffer) - 1);
        buffer[sizeof(buffer) - 1] = 0;
        if (ImGui::InputTextMultiline("##bt_json", buffer, sizeof(buffer),
                                      ImVec2(-1.0F, ImGui::GetTextLineHeight() * 8.0F))) {
            bt.source = buffer;
        }
        if (ImGui::Button("Recompile", ImVec2(-1.0F, 0.0F))) {
            bt.compiled.reset();
            bt.parse_failed = false;
        }
        if (bt.parse_failed) {
            ImGui::TextColored(ImVec4(1.0F, 0.4F, 0.3F, 1.0F), "parse failed (see log)");
        } else if (bt.compiled != nullptr) {
            ImGui::Text("compiled: %zu nodes", bt.compiled->nodes().size());
        }
        ImGui::TextWrapped("JSON: sequence/selector/inverter over leaves "
                           "move_to, wait, near_point, near_camera.");
    }
};

template <>
struct ComponentInspector<scene::Camera> {
    static constexpr const char* title = "Camera";
    static void draw(entt::registry&, entt::entity, scene::Camera& camera) {
        float fov_deg = glm::degrees(camera.fov_y);
        if (ImGui::SliderFloat("fov (deg)", &fov_deg, 10.0F, 140.0F)) {
            camera.fov_y = glm::radians(fov_deg);
        }
        ImGui::DragFloat("near", &camera.near_z, 0.01F, 0.001F, 100.0F);
        ImGui::DragFloat("far", &camera.far_z, 1.0F, 0.1F, 100000.0F);
        ImGui::Checkbox("active", &camera.is_active);
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
        ImGui::Text("time: %.2fs", animator.time);
        ImGui::DragFloat("speed", &animator.speed, 0.05F, -4.0F, 4.0F);
        ImGui::Checkbox("looping", &animator.looping);
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
    inspect<scene::DirectionalLight>(r, selected, true);
    inspect<scene::PointLight>(r, selected, true);
    inspect<scene::Camera>(r, selected, true);
    inspect<scene::Animator>(r, selected);
    inspect<scene::Terrain>(r, selected, true);
    inspect<scene::NavAgent>(r, selected, true);
    inspect<scene::BehaviorTree>(r, selected, true);
    inspect<scene::Collider>(r, selected, true);
    inspect<scene::RigidBody>(r, selected, true);

    ImGui::Separator();
    if (ImGui::Button("Add component", ImVec2(-1.0F, 0.0F))) {
        ImGui::OpenPopup("add_component");
    }
    if (ImGui::BeginPopup("add_component")) {
        add_component_item<scene::Collider>(r, selected);
        add_component_item<scene::RigidBody>(r, selected);
        add_component_item<scene::Terrain>(r, selected);
        add_component_item<scene::NavAgent>(r, selected);
        add_component_item<scene::BehaviorTree>(r, selected);
        add_component_item<scene::PointLight>(r, selected, glm::vec3(1.0F), 10.0F, 5.0F);
        add_component_item<scene::DirectionalLight>(
            r, selected, glm::vec3(-0.4F, -1.0F, -0.3F), glm::vec3(1.0F), 3.0F);
        add_component_item<scene::Camera>(r, selected);
        ImGui::EndPopup();
    }
    ImGui::End();
}

void draw_renderer_panel(RendererSettings& settings, scene::Scene* scene) {
    if (!ImGui::Begin("Renderer")) {
        ImGui::End();
        return;
    }

    if (ImGui::CollapsingHeader("Sun", ImGuiTreeNodeFlags_DefaultOpen)) {
        scene::DirectionalLight* sun = nullptr;
        if (scene != nullptr) {
            for (const entt::entity e : scene->registry().view<scene::DirectionalLight>()) {
                sun = &scene->registry().get<scene::DirectionalLight>(e);
                break;
            }
        }
        if (sun == nullptr) {
            ImGui::TextUnformatted("(no DirectionalLight in the scene)");
        } else {
            // direction = travel direction (sun -> scene), unit length.
            const glm::vec3 dir = glm::normalize(sun->direction);
            float azimuth = glm::degrees(std::atan2(dir.z, dir.x));
            float elevation = glm::degrees(std::asin(glm::clamp(-dir.y, -1.0F, 1.0F)));

            bool changed = false;
            changed |= ImGui::SliderFloat("azimuth", &azimuth, -180.0F, 180.0F, "%.0f deg");
            changed |= ImGui::SliderFloat("elevation", &elevation, 2.0F, 89.0F, "%.0f deg");
            if (changed) {
                const float az = glm::radians(azimuth);
                const float el = glm::radians(glm::clamp(elevation, 2.0F, 89.0F));
                sun->direction = glm::vec3(std::cos(el) * std::cos(az), -std::sin(el),
                                           std::cos(el) * std::sin(az));
            }
            ImGui::ColorEdit3("colour", &sun->color.x);
            ImGui::DragFloat("intensity", &sun->intensity, 0.05F, 0.0F, 100.0F);
        }
    }

    if (ImGui::CollapsingHeader("Clouds", ImGuiTreeNodeFlags_DefaultOpen)) {
        renderer::CloudSettings& c = settings.clouds;
        ImGui::Checkbox("enabled", &c.enabled);
        ImGui::BeginDisabled(!c.enabled);
        ImGui::SliderFloat("coverage", &c.coverage, 0.0F, 1.0F, "%.2f");
        ImGui::SliderFloat("density", &c.density, 2.0F, 64.0F, "%.0f /km");
        ImGui::SliderFloat("altitude", &c.bottom_km, 0.5F, 10.0F, "%.1f km");
        ImGui::SliderFloat("thickness", &c.thickness_km, 0.5F, 8.0F, "%.1f km");
        ImGui::SliderFloat("puff size", &c.size_km, 2.0F, 40.0F, "%.0f km");
        ImGui::SliderFloat("detail", &c.detail, 0.0F, 1.0F, "%.2f");
        ImGui::SliderFloat("wind speed", &c.wind_speed, 0.0F, 80.0F, "%.0f m/s");
        ImGui::SliderFloat("wind direction", &c.wind_dir_deg, -180.0F, 180.0F, "%.0f deg");
        ImGui::EndDisabled();
    }

    if (ImGui::CollapsingHeader("Bloom", ImGuiTreeNodeFlags_DefaultOpen)) {
        ImGui::SliderFloat("threshold", &settings.bloom_threshold, 0.0F, 8.0F);
        ImGui::SliderFloat("knee", &settings.bloom_knee, 0.0F, 1.0F);
        ImGui::SliderFloat("intensity", &settings.bloom_intensity, 0.0F, 0.5F);
        ImGui::SliderFloat("radius", &settings.bloom_radius, 0.2F, 2.0F);
    }
    if (ImGui::CollapsingHeader("Environment", ImGuiTreeNodeFlags_DefaultOpen)) {
        ImGui::TextWrapped("The IBL environment (ambient + reflections) re-renders continuously "
                           "(one step per frame), so it tracks the sun and clouds by itself.");
    }
    ImGui::End();
}

} // namespace engine::editor
