#include "grass_inspector.hpp"

#include <cmath>
#include <cstdint>

#include <glm/glm.hpp>
#include <imgui.h>

#include "engine/scene/components.hpp"

namespace engine::editor {

namespace {

[[nodiscard]] float to_srgb(float c) {
    return c <= 0.0031308F ? c * 12.92F : 1.055F * std::pow(c, 1.0F / 2.4F) - 0.055F;
}

[[nodiscard]] float to_linear(float c) {
    return c <= 0.04045F ? c / 12.92F : std::pow((c + 0.055F) / 1.055F, 2.4F);
}

// The picker shows sRGB (what artists pick); the component stores linear.
void color_srgb(const char* label, glm::vec3& linear) {
    glm::vec3 srgb(to_srgb(linear.r), to_srgb(linear.g), to_srgb(linear.b));
    if (ImGui::ColorEdit3(label, &srgb.x)) {
        linear = glm::vec3(to_linear(srgb.r), to_linear(srgb.g), to_linear(srgb.b));
    }
}

} // namespace

void draw_grass(scene::Grass& grass) {
    renderer::GrassSettings& g = grass.settings;
    int seed = static_cast<int>(g.seed & 0x7FFFFFFFU);
    if (ImGui::DragInt("seed", &seed, 1.0F, 0, 0x7FFFFFFF)) g.seed = static_cast<std::uint32_t>(seed);
    ImGui::DragFloat("density", &g.density, 0.5F, 0.0F, 1000.0F, "%.1f / m2");
    ImGui::DragFloat("radius", &g.radius, 0.5F, 0.0F, 500.0F, "%.1f m");
    ImGui::DragFloatRange2("blade height", &g.height_range.x, &g.height_range.y, 0.01F, 0.01F,
                           5.0F, "%.2f m");
    ImGui::DragFloatRange2("blade width", &g.width_range.x, &g.width_range.y, 0.001F, 0.001F,
                           0.5F, "%.3f m");
    color_srgb("base colour", g.base_color);
    color_srgb("tip colour", g.tip_color);

    ImGui::SeparatorText("Wind");
    ImGui::SliderFloat("strength", &g.wind_strength, 0.0F, 2.0F);
    float heading = glm::degrees(std::atan2(g.wind_direction.y, g.wind_direction.x));
    if (ImGui::SliderFloat("direction", &heading, -180.0F, 180.0F, "%.0f deg")) {
        const float h = glm::radians(heading);
        g.wind_direction = glm::vec2(std::cos(h), std::sin(h));
    }
    ImGui::SliderFloat("frequency", &g.wind_frequency, 0.0F, 3.0F, "%.2f Hz");

    ImGui::SeparatorText("Where it grows");
    ImGui::DragFloatRange2("slope", &g.slope_deg.x, &g.slope_deg.y, 0.5F, 0.0F, 90.0F,
                           "%.0f deg");
    ImGui::DragFloatRange2("height", &g.height_m.x, &g.height_m.y, 1.0F, -100000.0F, 100000.0F,
                           "%.0f m");
    ImGui::Checkbox("terrain only", &g.terrain_only);
    if (!g.terrain_only) {
        ImGui::DragFloat("ground y", &g.ground_y, 0.05F, -100000.0F, 100000.0F, "%.2f m");
        ImGui::SetItemTooltip("Flat ground height where no terrain is loaded");
    }

    ImGui::SeparatorText("Level of detail");
    ImGui::SliderFloat("lod start", &g.lod_start, 0.0F, 1.0F, "%.2f x radius");
    ImGui::SetItemTooltip("Distance where blades start thinning out and lose segments");
    ImGui::SliderFloat("far density", &g.far_density, 0.0F, 1.0F);
    ImGui::SetItemTooltip("Fraction of blades kept at the radius");
}

} // namespace engine::editor
