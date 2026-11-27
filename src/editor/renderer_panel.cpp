// Renderer panel (sun, clouds, bloom, environment) on the widget kit.

#include <cmath>

#include <entt/entity/registry.hpp>
#include <glm/glm.hpp>
#include <imgui.h>

#include "engine/editor/editor.hpp"
#include "engine/scene/components.hpp"
#include "engine/scene/scene.hpp"
#include "inspector.hpp"
#include "shell/panels.hpp"
#include "ui/icons.hpp"
#include "ui/widgets.hpp"

namespace engine::editor {

namespace {

void hint(const char* text) {
    ImGui::PushTextWrapPos(0.0F);
    ui::text_colored(ui::tokens().text_secondary, text);
    ImGui::PopTextWrapPos();
}

} // namespace

void draw_renderer_panel(RendererSettings& settings, scene::Scene* scene) {
    if (!ImGui::Begin(shell::panel_window_name(shell::Panel::renderer))) {
        ImGui::End();
        return;
    }

    // Sections push their own IDs, so equally labelled widgets (the sun's and
    // bloom's "Intensity") stay distinct.
    if (ui::section_header(ui::label(icon::sun, "Sun").c_str())) {
        ImGui::PushID("sun");
        scene::DirectionalLight* sun = nullptr;
        if (scene != nullptr) {
            for (const entt::entity e : scene->registry().view<scene::DirectionalLight>()) {
                sun = &scene->registry().get<scene::DirectionalLight>(e);
                break;
            }
        }
        if (sun == nullptr) {
            hint("No Directional Light in the scene (Create > Directional Light).");
        } else if (ui::begin_property_grid("sun")) {
            // direction = travel direction (sun -> scene), unit length.
            const glm::vec3 dir = glm::normalize(sun->direction);
            float azimuth = glm::degrees(std::atan2(dir.z, dir.x));
            float elevation = glm::degrees(std::asin(glm::clamp(-dir.y, -1.0F, 1.0F)));

            bool changed = false;
            changed |= ui::prop_float("Azimuth", azimuth,
                                      {.min = -180.0F, .max = 180.0F, .format = "%.0f°", .slider = true});
            changed |= ui::prop_float("Elevation", elevation,
                                      {.min = 2.0F, .max = 89.0F, .format = "%.0f°", .slider = true});
            if (changed) {
                const float az = glm::radians(azimuth);
                const float el = glm::radians(glm::clamp(elevation, 2.0F, 89.0F));
                sun->direction = glm::vec3(std::cos(el) * std::cos(az), -std::sin(el),
                                           std::cos(el) * std::sin(az));
            }
            (void)ui::prop_color3("Colour", sun->color, {.reset = glm::vec3(1.0F)});
            (void)ui::prop_float("Intensity", sun->intensity,
                                 {.speed = 0.05F, .min = 0.0F, .max = 100.0F});
            ui::end_property_grid();
        }
        ImGui::PopID();
    }

    if (ui::section_header(ui::label(icon::cloud, "Clouds").c_str())) {
        ImGui::PushID("clouds");
        renderer::CloudSettings& c = settings.clouds;
        if (ui::begin_property_grid("clouds")) {
            (void)ui::prop_bool("Enabled", c.enabled);
            ImGui::BeginDisabled(!c.enabled);
            const auto slider = [](const char* label, float& v, float lo, float hi, const char* fmt) {
                (void)ui::prop_float(label, v, {.min = lo, .max = hi, .format = fmt, .slider = true});
            };
            slider("Coverage", c.coverage, 0.0F, 1.0F, "%.2f");
            slider("Density", c.density, 2.0F, 64.0F, "%.0f /km");
            slider("Altitude", c.bottom_km, 0.5F, 10.0F, "%.1f km");
            slider("Thickness", c.thickness_km, 0.5F, 8.0F, "%.1f km");
            slider("Puff size", c.size_km, 2.0F, 40.0F, "%.0f km");
            slider("Detail", c.detail, 0.0F, 1.0F, "%.2f");
            slider("Wind speed", c.wind_speed, 0.0F, 80.0F, "%.0f m/s");
            slider("Wind direction", c.wind_dir_deg, -180.0F, 180.0F, "%.0f°");
            ImGui::EndDisabled();
            ui::end_property_grid();
        }
        ImGui::PopID();
    }

    if (ui::section_header(ui::label(icon::sparkles, "Bloom").c_str())) {
        ImGui::PushID("bloom");
        if (ui::begin_property_grid("bloom")) {
            const RendererSettings defaults;
            (void)ui::prop_float("Threshold", settings.bloom_threshold,
                                 {.min = 0.0F, .max = 8.0F, .slider = true},
                                 {.reset = defaults.bloom_threshold});
            (void)ui::prop_float("Knee", settings.bloom_knee, {.min = 0.0F, .max = 1.0F, .slider = true},
                                 {.reset = defaults.bloom_knee});
            (void)ui::prop_float("Intensity", settings.bloom_intensity,
                                 {.min = 0.0F, .max = 0.5F, .slider = true},
                                 {.reset = defaults.bloom_intensity});
            (void)ui::prop_float("Radius", settings.bloom_radius,
                                 {.min = 0.2F, .max = 2.0F, .slider = true},
                                 {.reset = defaults.bloom_radius});
            ui::end_property_grid();
        }
        ImGui::PopID();
    }
    if (ui::section_header(ui::label(icon::globe, "Environment").c_str())) {
        hint("The IBL environment (ambient + reflections) re-renders continuously (one step per "
             "frame), so it tracks the sun and clouds by itself.");
    }
    ImGui::End();
}

} // namespace engine::editor
