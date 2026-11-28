#include "tuning_panels.hpp"
#include "weather_panel.hpp"

#include <cmath>
#include <cstddef>
#include <cstdio>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include <entt/entity/registry.hpp>
#include <glm/glm.hpp>
#include <imgui.h>
#include <imgui_stdlib.h>

#include "engine/renderer/physical_camera.hpp"
#include "engine/core/utf8_path.hpp"
#include "engine/renderer/render_settings.hpp"
#include "engine/scene/components.hpp"
#include "engine/scene/material_override.hpp"
#include "engine/scene/scene.hpp"
#include "time_of_day_panel.hpp"
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

// A `path` field (R20 colour-grading LUT): a text row committed on Enter
// when it passes the table's path check. The renderer loads and validates
// the file itself; a bad file is reported on stderr and leaves the image
// ungraded. A rejected entry keeps its error under the row.
void draw_path_setting(RenderSettings& s, const RenderSettingDesc& d, const std::string& label,
                       const std::string& hint, std::string& value) {
    struct Edit {
        const RenderSettingDesc* field = nullptr;
        std::string              error;
    };
    static std::vector<Edit> edits;
    Edit* e = nullptr;
    for (Edit& candidate : edits) {
        if (candidate.field == &d) e = &candidate;
    }
    if (e == nullptr) e = &edits.emplace_back(Edit{&d, {}});
    std::string entered = value;
    ui::PropOpts<std::string> opts;
    opts.reset = std::string{};
    opts.tooltip = hint.empty() ? nullptr : hint.c_str();
    if (ui::prop_text(label.c_str(), entered, opts, /*commit_on_enter=*/true)) {
        e->error = renderer::check_render_setting_path(d, entered);
        if (e->error.empty()) (void)renderer::set_render_setting_text(s, d, entered);
    }
    if (!e->error.empty()) ui::prop_note(e->error.c_str(), ui::tokens().error);
}

// One settings-table row: label, slider / checkbox, reset arrow when the
// value differs from the table's default.
void draw_setting(RenderSettings& s, const RenderSettingDesc& d) {
    const RenderSettingRef ref = d.ref(s);
    const std::string label = sentence_case(d.label);
    const std::string hint(d.hint);
    if (ref.s != nullptr) {
        draw_path_setting(s, d, label, hint, *ref.s);
        return;
    }
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
    if (d.group != "exposure" || d.name == "compensation_ev" || d.name == "physical") return false;
    // The physical camera overrides auto exposure; manual uses only the bias.
    if (s.exposure.physical) return true;
    return !s.exposure.auto_exposure && d.name != "auto";
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
    // While the world clock drives the light, its angles and strength come
    // from the clock (Time of Day below): shown, not editable.
    const bool driven = clock_drives_sun(scene->registry());
    if (driven) ui::prop_note("Driven by the world clock (Time of Day).");
    ImGui::BeginDisabled(driven);
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
    ImGui::EndDisabled();
    (void)ui::prop_color3("Colour", sun->color, {.reset = glm::vec3(1.0F)});
    ImGui::BeginDisabled(driven);
    (void)ui::prop_float("Intensity", sun->intensity, {.speed = 0.05F, .min = 0.0F, .max = 100.0F});
    ImGui::EndDisabled();
    ui::end_property_grid();
}

// The HDRI file row of the "Sky & HDRI" section (R21): a project-relative
// .hdr path (typed, browsed or dropped from the asset browser; the reset
// arrow clears it) and the load status (resolution and sun, or the error).
void draw_hdri_file(const HdriPanel& hdri) {
    if (hdri.request == nullptr) return;
    static constexpr std::string_view k_extensions[] = {".hdr"};
    std::string path = hdri.path;
    ui::PropOpts<std::string> opts;
    opts.reset = std::string();
    opts.tooltip = "Radiance .hdr panorama, relative to the project root";
    if (ui::prop_asset("HDRI file", path,
                       {.root = core::path_from_utf8(hdri.project_root), .extensions = k_extensions},
                       opts)) {
        *hdri.request = path;
    }
    if (!hdri.status.empty()) ui::prop_note(hdri.status.c_str());
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

// One lens row: a fill-bar slider with the reset arrow back to the
// PhysicalCamera default.
bool lens_slider(const char* label, const char* hint, float& v, float min, float max,
                 const char* format, bool log, float reset) {
    ImGuiSliderFlags flags = ImGuiSliderFlags_AlwaysClamp;
    if (log) flags |= ImGuiSliderFlags_Logarithmic;
    const bool modified = v != reset;
    ui::prop_begin(label, hint, true, modified);
    bool changed = ui::slider_float("##v", &v, min, max, format, flags);
    if (ui::prop_end(true, modified)) {
        v = reset;
        changed = true;
    }
    return changed;
}

} // namespace

bool draw_lens_controls(renderer::PhysicalCamera& lens, float fov_y) {
    using L = renderer::PhysicalCameraLimits;
    const renderer::PhysicalCamera defaults{};
    bool changed = false;
    changed |= lens_slider("Aperture", "Lower f-number: brighter and shallower depth of field",
                           lens.aperture_f, 1.0F, 22.0F, "f/%.1f", true, defaults.aperture_f);
    // Shown as the denominator of 1/x s, the photographer's notation.
    float denominator = 1.0F / lens.shutter_s;
    if (lens_slider("Shutter", "Exposure time; also the motion blur length (shutter angle)",
                    denominator, 1.0F / L::max_shutter, 8000.0F, "1/%.0f s", true,
                    1.0F / defaults.shutter_s)) {
        lens.shutter_s = 1.0F / denominator;
        changed = true;
    }
    changed |= lens_slider("ISO", "Sensor sensitivity", lens.iso, 50.0F, 12800.0F, "%.0f", true,
                           defaults.iso);
    changed |= lens_slider("Focal length", "> 0 sets the field of view from the sensor size",
                           lens.focal_length_mm, 0.0F, 600.0F,
                           lens.focal_length_mm > 0.0F ? "%.0f mm" : "Off (uses FOV)", false,
                           defaults.focal_length_mm);
    changed |= lens_slider("Sensor height", "Vertical sensor size", lens.sensor_height_mm, 4.0F,
                           60.0F, "%.1f mm", false, defaults.sensor_height_mm);
    changed |= lens_slider("Focus distance", "Distance that is sharp with depth of field on",
                           lens.focus_distance_m, L::min_focus_m, 1000.0F, "%.2f m", true,
                           defaults.focus_distance_m);
    char summary[64];
    (void)std::snprintf(summary, sizeof(summary), "EV100 %.1f, %.0f mm", renderer::ev100(lens),
                        renderer::effective_focal_length_mm(lens, fov_y));
    ui::prop_value("Exposure", summary, "Resulting exposure value and effective focal length");
    return changed;
}

void draw_renderer_panel(RenderSettings& settings, scene::Scene* scene, const HdriPanel& hdri,
                         renderer::PhysicalCamera* view_lens, float view_fov_y) {
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
    if (ui::section_header(ui::label(icon::clock_3, "Time of Day").c_str())) {
        ImGui::PushID("time_of_day");
        draw_time_of_day_section(scene);
        ImGui::PopID();
    }
    if (ui::section_header(ui::label(icon::cloud, "Weather").c_str())) {
        ImGui::PushID("weather");
        draw_weather_section(scene); // R13.1
        ImGui::PopID();
    }
    if (view_lens != nullptr) {
        const bool open = ui::section_header(ui::label(icon::camera, "View camera lens").c_str());
        ui::item_tooltip("The editor camera's lens: depth of field, motion blur and physical "
                         "exposure");
        if (open) {
            ImGui::PushID("view_lens");
            if (ui::begin_property_grid("lens")) {
                (void)draw_lens_controls(*view_lens, view_fov_y);
                ui::end_property_grid();
            }
            ImGui::PopID();
        }
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
            if (g.name == "environment") draw_hdri_file(hdri);
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
