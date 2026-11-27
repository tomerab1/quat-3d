#include "upscaler_inspector.hpp"

#include <array>
#include <string>

#include <imgui.h>

#include "engine/renderer/upscaler_settings.hpp"

namespace engine::editor {

void draw_upscaler_settings(renderer::UpscalerSettings& settings) {
    constexpr std::array<renderer::UpscalerMode, 4> modes{
        renderer::UpscalerMode::native_aa, renderer::UpscalerMode::quality,
        renderer::UpscalerMode::balanced, renderer::UpscalerMode::performance};
    const std::string current(renderer::to_string(settings.mode));
    if (ImGui::BeginCombo("mode", current.c_str())) {
        for (const renderer::UpscalerMode m : modes) {
            const std::string name(renderer::to_string(m));
            if (ImGui::Selectable(name.c_str(), m == settings.mode)) settings.mode = m;
        }
        ImGui::EndCombo();
    }
    ImGui::SliderFloat("scale override", &settings.render_scale, 0.0F, 1.0F,
                       settings.render_scale > 0.0F ? "%.2f" : "off (mode)");
    ImGui::Text("render scale %.2f",
                static_cast<double>(renderer::upscaler_render_scale(settings)));
    ImGui::SliderFloat("sharpness", &settings.sharpness, 0.0F, 1.0F, "%.2f");
    ImGui::TextUnformatted("backend: taau (FSR 2 not built yet)");
}

} // namespace engine::editor
