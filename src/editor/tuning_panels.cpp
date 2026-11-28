#include "tuning_panels.hpp"
#include "weather_panel.hpp"

#include <cmath>
#include <cstddef>
#include <optional>
#include <string>
#include <string_view>

#include <entt/entity/registry.hpp>
#include <glm/glm.hpp>
#include <imgui.h>

#include "engine/renderer/render_settings.hpp"
#include "engine/scene/components.hpp"
#include "engine/scene/material_override.hpp"
#include "engine/scene/scene.hpp"
#include "shell/panels.hpp"
#include "ui/icons.hpp"
#include "ui/widgets.hpp"

namespace engine::editor {

namespace {

using renderer::RenderSettingDesc;
using renderer::RenderSettingRef;
using renderer::RenderSettings;

// Table labels are lower case ("index of refraction"); the property grid
// capitalises its labels.
[[nodiscard]] std::string sentence_case(std::string_view label) {
    std::string out(label);
    if (!out.empty() && out[0] >= 'a' && out[0] <= 'z') {
        out[0] = static_cast<char>(out[0] - 'a' + 'A');
    }
    return out;
}

// Ranges spanning orders of magnitude get a logarithmic slider.
[[nodiscard]] bool logarithmic(float min, float max) { return min > 0.0F && max / min >= 100.0F; }

// printf format for a slider over [min, max] with an optional unit suffix.
[[nodiscard]] std::string slider_format(float min, float max, std::string_view unit) {
    const float span = max - min;
    std::string f = logarithmic(min, max) ? "%.3g"
                    : span <= 0.1F        ? "%.4f"
                    : span <= 2.0F        ? "%.2f"
                    : span <= 20.0F       ? "%.1f"
                                          : "%.0f";
    if (!unit.empty()) f += " " + std::string(unit);
    return f;
}

// One settings-table row: label, slider / checkbox, reset arrow when the
// value differs from the table's default.
void draw_setting(RenderSettings& s, const RenderSettingDesc& d) {
    const RenderSettingRef ref = d.ref(s);
    const std::string label = sentence_case(d.label);
    const std::string hint(d.hint);
    const std::string format = slider_format(d.min, d.max, d.unit);
    ImGuiSliderFlags flags = ImGuiSliderFlags_AlwaysClamp;
    if (logarithmic(d.min, d.max)) flags |= ImGuiSliderFlags_Logarithmic;
    const bool modified = renderer::render_setting_value(s, d) != d.default_value;
    ui::prop_begin(label.c_str(), hint.empty() ? nullptr : hint.c_str(), true, modified);
    if (ref.b != nullptr) {
        ImGui::Checkbox("##v", ref.b);
    } else if (d.type == renderer::RenderSettingType::choice && ref.i != nullptr) {
        // A named choice (e.g. the upscaler mode, R11).
        const std::string current(
            renderer::render_setting_choice_name(d, static_cast<float>(*ref.i)));
        ImGui::SetNextItemWidth(-1.0F);
        if (ImGui::BeginCombo("##v", current.c_str())) {
            for (std::size_t k = 0; k < d.choices.size(); ++k) {
                const std::string name(d.choices[k]);
                if (ImGui::Selectable(name.c_str(), static_cast<int>(k) == *ref.i)) {
                    *ref.i = static_cast<int>(k);
                }
            }
            ImGui::EndCombo();
        }
    } else if (ref.i != nullptr) {
        // Integers print as integers (a float format would read garbage).
        const std::string int_format = d.unit.empty() ? "%d" : "%d " + std::string(d.unit);
        (void)ui::slider_int("##v", ref.i, static_cast<int>(d.min), static_cast<int>(d.max),
                             int_format.c_str(), flags);
    } else {
        (void)ui::slider_float("##v", ref.f, d.min, d.max, format.c_str(), flags);
    }
    if (ui::prop_end(true, modified)) renderer::set_render_setting(s, d, d.default_value);
}

// Fields that have no effect in the current state are shown disabled: the
// rest of a group whose `enabled` is off, and the auto-exposure parameters
// while exposure is manual.
[[nodiscard]] bool inactive(const RenderSettings& s, const RenderSettingDesc& d) {
    if (d.name == "enabled") return false;
    if (const RenderSettingDesc* enabled = renderer::find_render_setting(d.group, "enabled");
        enabled != nullptr && renderer::render_setting_value(s, *enabled) == 0.0F) {
        return true;
    }
    // Upscaling needs the temporal resolve (TAA off renders at native scale).
    if (d.group == "upscaler" && !s.taa.enabled) return true;
    return d.group == "exposure" && !s.exposure.auto_exposure && d.name != "auto" &&
           d.name != "compensation_ev";
}

void draw_sun(scene::Scene* scene) {
    scene::DirectionalLight* sun = nullptr;
    if (scene != nullptr) {
        for (const entt::entity e : scene->registry().view<scene::DirectionalLight>()) {
            sun = &scene->registry().get<scene::DirectionalLight>(e);
            break;
        }
    }
    if (sun == nullptr) {
        ui::text_secondary("No Directional Light in the scene (Create > Directional Light).");
        return;
    }
    if (!ui::begin_property_grid("sun")) return;
    // direction = travel direction (sun -> scene), unit length; same angle
    // convention as environment.get / environment.set.
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
        sun->direction =
            glm::vec3(std::cos(el) * std::cos(az), -std::sin(el), std::cos(el) * std::sin(az));
    }
    (void)ui::prop_color3("Colour", sun->color, {.reset = glm::vec3(1.0F)});
    (void)ui::prop_float("Intensity", sun->intensity, {.speed = 0.05F, .min = 0.0F, .max = 100.0F});
    ui::end_property_grid();
}

// One material factor row; the reset arrow reverts an entity override.
void draw_material_field(entt::registry& r, entt::entity e, const scene::MaterialFieldInfo& f,
                         const asset::PbrMaterialParams& params, bool overridden) {
    const std::string label = sentence_case(f.label);
    const std::string hint(f.hint);
    glm::vec4 v = scene::material_field_value(params, f);
    ui::prop_begin(label.c_str(), hint.empty() ? nullptr : hint.c_str(), true, overridden);
    bool changed = false;
    switch (f.kind) {
    case scene::MaterialFieldKind::color4:
        changed = ui::color_field("##v", &v.x, true);
        break;
    case scene::MaterialFieldKind::color3:
        changed = ui::color_field("##v", &v.x, false, f.max > 1.0F);
        break;
    case scene::MaterialFieldKind::scalar:
        if (f.max - f.min > 100.0F) {
            changed = ImGui::DragFloat("##v", &v.x, 0.01F, f.min, f.max, "%.3f",
                                       ImGuiSliderFlags_AlwaysClamp);
        } else {
            changed = ui::slider_float("##v", &v.x, f.min, f.max,
                                       slider_format(f.min, f.max, {}).c_str(),
                                       ImGuiSliderFlags_AlwaysClamp);
        }
        break;
    }
    if (changed) scene::set_entity_material_field(r, e, f, v);
    // An edit may have (re)created the override; the arrow acts next frame.
    if (ui::prop_end(true, overridden) && !changed) scene::clear_material_override_field(r, e, f);
}

} // namespace

void draw_renderer_panel(RenderSettings& settings, scene::Scene* scene) {
    if (!ImGui::Begin(shell::panel_window_name(shell::Panel::renderer).c_str())) {
        ImGui::End();
        return;
    }
    // Sections push their own IDs, so equally labelled widgets (the sun's
    // and bloom's "Intensity") stay distinct.
    if (ui::section_header(ui::label(icon::sun, "Sun").c_str())) {
        ImGui::PushID("sun");
        draw_sun(scene);
        ImGui::PopID();
    }
    if (ui::section_header(ui::label(icon::cloud, "Weather").c_str())) {
        ImGui::PushID("weather");
        draw_weather_section(scene); // R13.1
        ImGui::PopID();
    }
    for (const renderer::RenderSettingGroup& g : renderer::render_setting_groups()) {
        const std::string header(g.label);
        const bool open = ui::section_header(header.c_str());
        if (!g.hint.empty()) ui::item_tooltip(std::string(g.hint).c_str());
        if (!open) continue;
        const std::string id(g.name);
        ImGui::PushID(id.c_str());
        if (ui::begin_property_grid("group")) {
            for (const RenderSettingDesc& d : renderer::render_setting_fields()) {
                if (d.group != g.name) continue;
                ImGui::BeginDisabled(inactive(settings, d));
                draw_setting(settings, d);
                ImGui::EndDisabled();
            }
            ui::prop_begin("", nullptr, false, false);
            if (ui::button(ui::label(icon::rotate_ccw, "Reset group").c_str(),
                           ui::ButtonKind::ghost)) {
                (void)renderer::reset_render_settings(settings, g.name);
            }
            (void)ui::prop_end(false, false);
            ui::end_property_grid();
        }
        ImGui::PopID();
    }
    ImGui::End();
}

void draw_material_inspector(entt::registry& r, entt::entity e) {
    if (r.all_of<scene::InstancedMesh>(e)) return; // its parts have their own materials
    const std::optional<asset::PbrMaterialParams> params = scene::effective_material_params(r, e);
    if (!params) return;
    const auto* o = r.try_get<scene::MaterialOverride>(e);
    const ui::ComponentHeader h = ui::component_header({
        .id = "material_tuning",
        .icon = icon::palette,
        .title = "Material",
        .can_reset = o != nullptr,
        .can_copy = false,
        .removable = false,
        .tooltip = "The drawn material's PBR factors; edits are per-entity overrides",
    });
    if (h.action == ui::HeaderAction::reset) r.remove<scene::MaterialOverride>(e);
    if (!h.open) return;
    // A primitive owns its colour, metallic, roughness and emission (its
    // component fields): they are edited there, not here.
    const bool primitive = r.all_of<scene::Primitive>(e);
    ImGui::PushID("material_tuning");
    if (ui::begin_property_grid("fields")) {
        for (const scene::MaterialFieldInfo& f : scene::material_fields()) {
            if (primitive && f.primitive_owned) continue;
            const auto* ov = r.try_get<scene::MaterialOverride>(e);
            const bool overridden = ov != nullptr && (ov->mask & f.bit) != 0;
            const std::string id(f.name);
            ImGui::PushID(id.c_str());
            draw_material_field(r, e, f, *params, overridden);
            ImGui::PopID();
        }
        if (r.all_of<scene::MaterialOverride>(e)) {
            ui::prop_note("Reset arrows revert a factor to the material's own value; the "
                          "header's ⋮ menu reverts them all.");
        }
        ui::end_property_grid();
    }
    ImGui::PopID();
}

} // namespace engine::editor
