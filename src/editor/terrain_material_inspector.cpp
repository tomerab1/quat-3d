// Inspector rows for the layered terrain material (ROADMAP R26.1): preset,
// global blending / tiling / distance settings and the layer stack (top
// first, as it composites), each layer's texture set, tint, tiling and its
// placement rules. Stack edits go through the plain functions in
// engine/terrain/material.hpp (tested without ImGui).

#include "terrain_material_inspector.hpp"

#include <cstddef>
#include <string>

#include <glm/glm.hpp>
#include <imgui.h>

#include "engine/terrain/material.hpp"
#include "ui/icons.hpp"
#include "ui/widgets.hpp"

namespace engine::editor {

namespace {

constexpr ui::EnumItem k_altitude_refs[] = {
    {static_cast<int>(terrain::AltitudeRef::world), "World height", nullptr},
    {static_cast<int>(terrain::AltitudeRef::snowline), "Above snowline",
     "Heights relative to the terrain's snowline"},
};

constexpr ui::EnumItem k_paint_channels[] = {
    {static_cast<int>(terrain::PaintChannel::none), "None", nullptr},
    {static_cast<int>(terrain::PaintChannel::r), "Dirt (R)", "Terrain edit paint channel R"},
    {static_cast<int>(terrain::PaintChannel::g), "Gravel (G)", "Terrain edit paint channel G"},
    {static_cast<int>(terrain::PaintChannel::b), "Soil (B)", "Terrain edit paint channel B"},
};

// Combo over the built-in texture sets; true when changed.
bool texture_set_combo(const char* label, std::string& set) {
    ui::prop_begin(label, "Built-in CC0 texture set (assets/terrain/layers.json)", false, false);
    bool changed = false;
    if (ImGui::BeginCombo("##set", set.c_str())) {
        for (const std::string_view name : terrain::texture_set_names) {
            const bool selected = name == set;
            if (ImGui::Selectable(std::string(name).c_str(), selected)) {
                set = std::string(name);
                changed = true;
            }
            if (selected) ImGui::SetItemDefaultFocus();
        }
        ImGui::EndCombo();
    }
    (void)ui::prop_end(false, false);
    return changed;
}

void draw_rule(terrain::LayerRule& r) {
    (void)ui::prop_range("Slope", r.slope_deg,
                         {.speed = 0.5F, .min = 0.0F, .max = 90.0F, .format = "%.0f°"});
    (void)ui::prop_float("Slope softness", r.slope_softness_deg,
                         {.speed = 0.2F, .min = 0.0F, .max = 45.0F, .format = "%.1f°"});
    (void)ui::prop_enum("Altitude from", r.altitude_ref, k_altitude_refs);
    (void)ui::prop_range("Altitude", r.altitude_m,
                         {.speed = 1.0F, .min = -100000.0F, .max = 100000.0F, .format = "%.0f m"},
                         {.tooltip = "Beyond +-99999 m an edge is open"});
    (void)ui::prop_float("Altitude softness", r.altitude_softness_m,
                         {.speed = 0.5F, .min = 0.0F, .max = 10000.0F, .format = "%.1f m"});
    (void)ui::prop_float("Concavity", r.concavity,
                         {.min = -1.0F, .max = 1.0F, .format = "%.2f", .slider = true},
                         {.reset = 0.0F, .tooltip = "> 0 hollows and gullies, < 0 ridges"});
    (void)ui::prop_float("Flow", r.flow,
                         {.min = -1.0F, .max = 1.0F, .format = "%.2f", .slider = true},
                         {.reset = 0.0F, .tooltip = "> 0 where water collects, < 0 dry ground"});
    (void)ui::prop_float("Noise", r.noise,
                         {.min = 0.0F, .max = 1.0F, .format = "%.2f", .slider = true},
                         {.tooltip = "Break-up of the band edges"});
    (void)ui::prop_float("Strength", r.strength,
                         {.min = 0.0F, .max = 1.0F, .format = "%.2f", .slider = true});
    (void)ui::prop_enum("Paint channel", r.paint, k_paint_channels);
}

// One layer; returns a stack action: 0 none, 1 up, -1 down, 2 remove.
int draw_layer(terrain::MaterialLayer& l, std::size_t index, std::size_t count) {
    int action = 0;
    ImGui::PushID(static_cast<int>(index));
    const std::string title = std::to_string(index) + "  " + l.name +
                              (index == 0 ? "  (base)" : "");
    ui::prop_separator(title.c_str());
    ui::prop_begin("", nullptr, false, false);
    if (index + 1 < count && ui::button(ui::label(icon::arrow_up, "Up").c_str(),
                                        ui::ButtonKind::secondary)) {
        action = 1;
    }
    ImGui::SameLine();
    if (index > 0 && ui::button(ui::label(icon::arrow_down, "Down").c_str(),
                                ui::ButtonKind::secondary)) {
        action = -1;
    }
    ImGui::SameLine();
    if (count > 1 && ui::button(ui::label(icon::trash_2, "Remove").c_str(),
                                ui::ButtonKind::secondary)) {
        action = 2;
    }
    (void)ui::prop_end(false, false);
    (void)ui::prop_text("Name", l.name);
    (void)texture_set_combo("Texture set", l.texture_set);
    (void)ui::prop_color3("Tint", l.tint, {.reset = glm::vec3(1.0F)});
    (void)ui::prop_float("Tile size", l.uv_scale_m,
                         {.speed = 0.05F, .min = 0.05F, .max = 1000.0F, .format = "%.2f m"});
    (void)ui::prop_float("Normal strength", l.normal_strength,
                         {.min = 0.0F, .max = 2.0F, .format = "%.2f", .slider = true},
                         {.reset = 1.0F});
    (void)ui::prop_float("Roughness", l.roughness,
                         {.min = 0.0F, .max = 2.0F, .format = "%.2f", .slider = true},
                         {.reset = 1.0F, .tooltip = "Multiplies the texture's roughness"});
    (void)ui::prop_float("Height offset", l.height_offset,
                         {.min = -1.0F, .max = 1.0F, .format = "%.2f", .slider = true},
                         {.reset = 0.0F, .tooltip = "> 0 pokes through neighbours in blends"});
    if (index > 0) {
        draw_rule(l.rule); // the base layer covers everything
    } else {
        (void)ui::prop_enum("Paint channel", l.rule.paint, k_paint_channels);
    }
    ImGui::PopID();
    return action;
}

} // namespace

void draw_terrain_material(terrain::TerrainMaterial& m) {
    ui::prop_separator("Material");
    ui::prop_begin("Preset", "Replace the material with a preset", false, false);
    if (ImGui::BeginCombo("##preset", m.preset.empty() ? "custom" : m.preset.c_str())) {
        for (const std::string_view name : terrain::material_preset_names) {
            if (ImGui::Selectable(std::string(name).c_str(), name == m.preset)) {
                if (auto preset = terrain::material_preset(name)) m = std::move(*preset);
            }
        }
        ImGui::EndCombo();
    }
    (void)ui::prop_end(false, false);
    (void)ui::prop_float("Blend depth", m.blend_depth,
                         {.min = 0.01F, .max = 1.0F, .format = "%.2f", .slider = true},
                         {.tooltip = "Height-blend transition: small = crisp, large = soft"});
    (void)ui::prop_float("Triplanar above", m.triplanar_slope_deg,
                         {.min = 0.0F, .max = 90.0F, .format = "%.0f°", .slider = true});
    (void)ui::prop_float("Triplanar sharpness", m.triplanar_sharpness,
                         {.min = 1.0F, .max = 16.0F, .format = "%.1f", .slider = true});
    (void)ui::prop_bool("Stochastic tiling", m.stochastic_tiling,
                        {.tooltip = "Hex-tiled texturing hides the texture repeat"});
    (void)ui::prop_float("Macro variation", m.macro_strength,
                         {.min = 0.0F, .max = 1.0F, .format = "%.2f", .slider = true});
    (void)ui::prop_float("Macro scale", m.macro_scale_m,
                         {.speed = 1.0F, .min = 1.0F, .max = 10000.0F, .format = "%.0f m"});
    (void)ui::prop_float("Far colour from", m.far_start_m,
                         {.speed = 1.0F, .min = 0.0F, .max = 100000.0F, .format = "%.0f m"});
    (void)ui::prop_float("Far colour over", m.far_range_m,
                         {.speed = 1.0F, .min = 1.0F, .max = 100000.0F, .format = "%.0f m"});

    // Top layer first (it composites over the ones below).
    std::size_t act_index = 0;
    int act = 0;
    for (std::size_t i = m.layers.size(); i-- > 0;) {
        const int a = draw_layer(m.layers[i], i, m.layers.size());
        if (a != 0) {
            act = a;
            act_index = i;
        }
    }
    if (act == 2) {
        (void)terrain::remove_material_layer(m, act_index);
    } else if (act != 0) {
        (void)terrain::move_material_layer(m, act_index, act);
    }
    ui::prop_begin("", nullptr, false, false);
    if (m.layers.size() < terrain::max_material_layers &&
        ui::button(ui::label(icon::plus, "Add layer").c_str(), ui::ButtonKind::secondary,
                   ImVec2(-1.0F, 0.0F))) {
        (void)terrain::add_material_layer(m, "rock");
    }
    (void)ui::prop_end(false, false);
}

} // namespace engine::editor
