#include "inspector.hpp"

#define GLM_ENABLE_EXPERIMENTAL
#include <cstring>
#include <string>

#include <glm/glm.hpp>
#include <glm/gtc/quaternion.hpp>
#include <glm/gtx/matrix_decompose.hpp>
#include <imgui.h>

#include "engine/editor/editor.hpp"
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

template <typename T>
void inspect(entt::registry& registry, entt::entity entity) {
    if (auto* component = registry.try_get<T>(entity)) {
        if (ImGui::CollapsingHeader(ComponentInspector<T>::title,
                                    ImGuiTreeNodeFlags_DefaultOpen)) {
            ImGui::PushID(ComponentInspector<T>::title);
            ComponentInspector<T>::draw(registry, entity, *component);
            ImGui::PopID();
        }
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
    inspect<scene::DirectionalLight>(r, selected);
    inspect<scene::PointLight>(r, selected);
    inspect<scene::Camera>(r, selected);
    inspect<scene::Animator>(r, selected);
    ImGui::End();
}

void draw_renderer_panel(RendererSettings& settings) {
    if (!ImGui::Begin("Renderer")) {
        ImGui::End();
        return;
    }
    if (ImGui::CollapsingHeader("Bloom", ImGuiTreeNodeFlags_DefaultOpen)) {
        ImGui::SliderFloat("threshold", &settings.bloom_threshold, 0.0F, 8.0F);
        ImGui::SliderFloat("knee", &settings.bloom_knee, 0.0F, 1.0F);
        ImGui::SliderFloat("intensity", &settings.bloom_intensity, 0.0F, 0.5F);
        ImGui::SliderFloat("radius", &settings.bloom_radius, 0.2F, 2.0F);
    }
    if (ImGui::CollapsingHeader("Environment", ImGuiTreeNodeFlags_DefaultOpen)) {
        ImGui::TextWrapped("Sun direction/colour: edit the DirectionalLight entity in the "
                           "Inspector. The IBL environment is baked once; rebake after "
                           "changing the sun.");
        if (ImGui::Button("Rebake IBL")) {
            settings.rebake_ibl = true;
        }
    }
    ImGui::End();
}

} // namespace engine::editor
