#include "material_inspector.hpp"

#include <cstring>
#include <string_view>

#include <imgui.h>

#include "engine/rhi/material_shader_names.hpp"
#include "engine/scene/components.hpp"

namespace engine::editor {

void draw_primitive_fields(scene::Primitive& p) {
    const char* shapes[] = {"cube", "sphere", "plane", "cylinder", "cone", "capsule"};
    int shape = static_cast<int>(p.shape);
    if (ImGui::Combo("shape", &shape, shapes, 6)) {
        p.shape = static_cast<scene::PrimitiveShape>(shape);
    }
    ImGui::ColorEdit4("color", &p.color.x);
    ImGui::SliderFloat("metallic", &p.metallic, 0.0F, 1.0F);
    ImGui::SliderFloat("roughness", &p.roughness, 0.0F, 1.0F);
    ImGui::ColorEdit3("emissive", &p.emissive.x, ImGuiColorEditFlags_HDR);

    // The name commits on Enter: every edit re-resolves the material.
    char shader[rhi::material_shaders::max_name_length + 1]{};
    std::strncpy(shader, p.shader.c_str(), sizeof(shader) - 1);
    if (ImGui::InputText("shader", shader, sizeof(shader),
                         ImGuiInputTextFlags_EnterReturnsTrue)) {
        const std::string_view name(shader);
        if (name.empty() || name == rhi::material_shaders::builtin_name) {
            p.shader.clear();
        } else if (rhi::material_shaders::valid_name(name)) {
            p.shader = shader;
        }
    }
    if (ImGui::IsItemHovered()) {
        ImGui::SetTooltip("Material shader: assets/shaders/materials/<name>.slang (Enter to "
                          "apply); empty = built-in PBR");
    }
    if (!p.shader.empty()) {
        ImGui::DragFloat4("shader params 0", &p.shader_params[0].x, 0.01F);
        ImGui::DragFloat4("shader params 1", &p.shader_params[1].x, 0.01F);
    }
}

} // namespace engine::editor
