#include "tuning_panels.hpp"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstddef>
#include <vector>
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

namespace engine::editor {

namespace {

using renderer::RenderSettingDesc;
using renderer::RenderSettingRef;
using renderer::RenderSettings;

void tooltip(std::string_view hint) {
    if (!hint.empty()) ImGui::SetItemTooltip("%.*s", static_cast<int>(hint.size()), hint.data());
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

// A `path` field: edited in a text box and committed on Enter when it passes
// the table's path check (the renderer loads and validates the file itself;
// a bad file is reported on stderr and leaves the image ungraded).
bool draw_path_setting(RenderSettings& s, const RenderSettingDesc& d, std::string& value) {
    struct Edit {
        const RenderSettingDesc* field = nullptr;
        std::array<char, 257>    text{};
        std::string              error;
        bool                     active = false; // being typed into last frame
    };
    static std::vector<Edit> edits;
    Edit* e = nullptr;
    for (Edit& candidate : edits) {
        if (candidate.field == &d) e = &candidate;
    }
    if (e == nullptr) e = &edits.emplace_back(Edit{&d, {}, {}, false});
    const std::string label(d.label);
    if (!e->active) { // show the current value unless the user is typing
        const std::size_t n = std::min(value.size(), e->text.size() - 1);
        std::copy_n(value.data(), n, e->text.data());
        e->text[n] = '\0';
    }
    bool changed = false;
    if (ImGui::InputText(label.c_str(), e->text.data(), e->text.size(),
                         ImGuiInputTextFlags_EnterReturnsTrue)) {
        e->active = false;
        const std::string_view entered(e->text.data());
        e->error = renderer::check_render_setting_path(d, entered);
        if (e->error.empty()) {
            changed = renderer::set_render_setting_text(s, d, entered);
        }
    } else {
        e->active = ImGui::IsItemActive();
    }
    tooltip(d.hint);
    if (!e->error.empty()) ImGui::TextColored(ImVec4(1.0F, 0.45F, 0.35F, 1.0F), "%s", e->error.c_str());
    return changed;
}

bool draw_setting(RenderSettings& s, const RenderSettingDesc& d) {
    const RenderSettingRef ref = d.ref(s);
    if (ref.s != nullptr) return draw_path_setting(s, d, *ref.s);
    const std::string label(d.label);
    const std::string format = slider_format(d.min, d.max, d.unit);
    ImGuiSliderFlags flags = ImGuiSliderFlags_AlwaysClamp;
    if (logarithmic(d.min, d.max)) flags |= ImGuiSliderFlags_Logarithmic;
    bool changed = false;
    if (ref.b != nullptr) {
        changed = ImGui::Checkbox(label.c_str(), ref.b);
    } else if (d.type == renderer::RenderSettingType::choice && ref.i != nullptr) {
        const std::string current(renderer::render_setting_choice_name(d, static_cast<float>(*ref.i)));
        if (ImGui::BeginCombo(label.c_str(), current.c_str())) {
            for (std::size_t k = 0; k < d.choices.size(); ++k) {
                const std::string name(d.choices[k]);
                if (ImGui::Selectable(name.c_str(), static_cast<int>(k) == *ref.i)) {
                    *ref.i = static_cast<int>(k);
                    changed = true;
                }
            }
            ImGui::EndCombo();
        }
    } else if (ref.i != nullptr) {
        changed = ImGui::SliderInt(label.c_str(), ref.i, static_cast<int>(d.min),
                                   static_cast<int>(d.max), format.c_str(), flags);
    } else {
        changed = ImGui::SliderFloat(label.c_str(), ref.f, d.min, d.max, format.c_str(), flags);
    }
    tooltip(d.hint);
    return changed;
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
        ImGui::TextUnformatted("(no DirectionalLight in the scene)");
        return;
    }
    // direction = travel direction (sun -> scene), unit length; same angle
    // convention as environment.get / environment.set.
    const glm::vec3 dir = glm::normalize(sun->direction);
    float azimuth = glm::degrees(std::atan2(dir.z, dir.x));
    float elevation = glm::degrees(std::asin(glm::clamp(-dir.y, -1.0F, 1.0F)));
    bool changed = false;
    changed |= ImGui::SliderFloat("azimuth", &azimuth, -180.0F, 180.0F, "%.0f deg");
    changed |= ImGui::SliderFloat("elevation", &elevation, 2.0F, 89.0F, "%.0f deg");
    if (changed) {
        const float az = glm::radians(azimuth);
        const float el = glm::radians(glm::clamp(elevation, 2.0F, 89.0F));
        sun->direction =
            glm::vec3(std::cos(el) * std::cos(az), -std::sin(el), std::cos(el) * std::sin(az));
    }
    ImGui::ColorEdit3("colour", &sun->color.x);
    ImGui::DragFloat("intensity", &sun->intensity, 0.05F, 0.0F, 100.0F);
}

bool draw_material_field(entt::registry& r, entt::entity e, const scene::MaterialFieldInfo& f,
                         const asset::PbrMaterialParams& params) {
    const std::string label(f.label);
    glm::vec4 v = scene::material_field_value(params, f);
    bool changed = false;
    switch (f.kind) {
    case scene::MaterialFieldKind::color4:
        changed = ImGui::ColorEdit4(label.c_str(), &v.x, ImGuiColorEditFlags_Float);
        break;
    case scene::MaterialFieldKind::color3: {
        ImGuiColorEditFlags flags = ImGuiColorEditFlags_Float;
        if (f.max > 1.0F) flags |= ImGuiColorEditFlags_HDR;
        changed = ImGui::ColorEdit3(label.c_str(), &v.x, flags);
        break;
    }
    case scene::MaterialFieldKind::scalar:
        if (f.max - f.min > 100.0F) {
            changed = ImGui::DragFloat(label.c_str(), &v.x, 0.01F, f.min, f.max, "%.3f",
                                       ImGuiSliderFlags_AlwaysClamp);
        } else {
            changed = ImGui::SliderFloat(label.c_str(), &v.x, f.min, f.max,
                                         slider_format(f.min, f.max, {}).c_str(),
                                         ImGuiSliderFlags_AlwaysClamp);
        }
        break;
    }
    tooltip(f.hint);
    if (changed) scene::set_entity_material_field(r, e, f, v);
    return changed;
}

} // namespace

void draw_renderer_panel(RenderSettings& settings, scene::Scene* scene) {
    if (!ImGui::Begin("Renderer")) {
        ImGui::End();
        return;
    }
    // Collapsing headers don't scope IDs: each section pushes its own, so
    // equally labelled widgets (the sun's and bloom's "intensity") stay
    // distinct.
    if (ImGui::CollapsingHeader("Sun", ImGuiTreeNodeFlags_DefaultOpen)) {
        ImGui::PushID("sun");
        draw_sun(scene);
        ImGui::PopID();
    }
    for (const renderer::RenderSettingGroup& g : renderer::render_setting_groups()) {
        const std::string header(g.label);
        const bool open = ImGui::CollapsingHeader(header.c_str(), ImGuiTreeNodeFlags_DefaultOpen);
        tooltip(g.hint);
        if (!open) continue;
        const std::string id(g.name);
        ImGui::PushID(id.c_str());
        for (const RenderSettingDesc& d : renderer::render_setting_fields()) {
            if (d.group != g.name) continue;
            ImGui::BeginDisabled(inactive(settings, d));
            draw_setting(settings, d);
            ImGui::EndDisabled();
        }
        if (ImGui::SmallButton("reset")) (void)renderer::reset_render_settings(settings, g.name);
        ImGui::PopID();
    }
    ImGui::End();
}

void draw_material_inspector(entt::registry& r, entt::entity e) {
    if (r.all_of<scene::InstancedMesh>(e)) return; // its parts have their own materials
    const std::optional<asset::PbrMaterialParams> params = scene::effective_material_params(r, e);
    if (!params) return;
    if (!ImGui::CollapsingHeader("Material", ImGuiTreeNodeFlags_DefaultOpen)) return;
    // A primitive owns its colour, metallic, roughness and emission (its
    // component fields): they are edited there, not here.
    const bool primitive = r.all_of<scene::Primitive>(e);
    ImGui::PushID("material_tuning");
    for (const scene::MaterialFieldInfo& f : scene::material_fields()) {
        if (primitive && f.primitive_owned) continue;
        const std::string id(f.name);
        ImGui::PushID(id.c_str());
        if (draw_material_field(r, e, f, *params)) {
            ImGui::PopID();
            continue; // the override may have been (re)created: re-read next frame
        }
        const auto* o = r.try_get<scene::MaterialOverride>(e);
        if (o != nullptr && (o->mask & f.bit) != 0) {
            ImGui::SameLine();
            if (ImGui::SmallButton("revert")) scene::clear_material_override_field(r, e, f);
            ImGui::SetItemTooltip("Overridden on this entity; revert to the material's value");
        }
        ImGui::PopID();
    }
    if (r.all_of<scene::MaterialOverride>(e) && ImGui::SmallButton("revert all")) {
        r.remove<scene::MaterialOverride>(e);
    }
    ImGui::PopID();
}

} // namespace engine::editor
