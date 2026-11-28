#include "weather_panel.hpp"

#include <string>
#include <string_view>

#include <entt/entity/registry.hpp>
#include <imgui.h>

#include "engine/scene/scene.hpp"
#include "engine/weather/weather_state.hpp"
#include "ui/widgets.hpp"

namespace engine::editor {

namespace {

using weather::WeatherFieldDesc;
using weather::WeatherFieldRef;
using weather::WeatherState;

// Table labels are lower case ("wind speed"); the property grid capitalises
// its labels.
[[nodiscard]] std::string sentence_case(std::string_view label) {
    std::string out(label);
    if (!out.empty() && out[0] >= 'a' && out[0] <= 'z') {
        out[0] = static_cast<char>(out[0] - 'a' + 'A');
    }
    return out;
}

[[nodiscard]] std::string slider_format(const WeatherFieldDesc& d) {
    const float span = d.max - d.min;
    std::string f = span <= 2.0F ? "%.2f" : span <= 20.0F ? "%.1f" : "%.0f";
    if (!d.unit.empty()) f += " " + std::string(d.unit);
    return f;
}

// The field's value as the table's number (checkbox 0 / 1, enum index).
[[nodiscard]] float field_value(const WeatherFieldRef& r) {
    if (r.b != nullptr) return *r.b ? 1.0F : 0.0F;
    if (r.p != nullptr) return static_cast<float>(*r.p);
    return *r.f;
}

void reset_field(const WeatherFieldRef& r, const WeatherFieldDesc& d) {
    if (r.b != nullptr) {
        *r.b = d.default_value != 0.0F;
    } else if (r.p != nullptr) {
        *r.p = static_cast<weather::PrecipitationType>(static_cast<int>(d.default_value));
    } else {
        *r.f = d.default_value;
    }
}

// One property row: label, widget, reset arrow when off its default.
void draw_field(WeatherState& s, const WeatherFieldDesc& d) {
    const WeatherFieldRef r = d.ref(s);
    const std::string label = sentence_case(d.label);
    const std::string hint(d.hint);
    const bool modified = field_value(r) != d.default_value;
    ui::prop_begin(label.c_str(), hint.empty() ? nullptr : hint.c_str(), true, modified);
    if (r.b != nullptr) {
        ImGui::Checkbox("##v", r.b);
    } else if (r.p != nullptr) {
        int current = static_cast<int>(*r.p);
        const char* items[] = {"rain", "snow"};
        ImGui::SetNextItemWidth(-1.0F);
        if (ImGui::Combo("##v", &current, items, 2)) {
            *r.p = static_cast<weather::PrecipitationType>(current);
        }
    } else {
        (void)ui::slider_float("##v", r.f, d.min, d.max, slider_format(d).c_str(),
                               ImGuiSliderFlags_AlwaysClamp);
    }
    if (ui::prop_end(true, modified)) reset_field(r, d);
}

} // namespace

void draw_weather_section(scene::Scene* scene) {
    if (scene == nullptr) return;
    entt::registry& r = scene->registry();
    const entt::entity e = weather::find_weather(r);
    if (e == entt::null) {
        ui::text_secondary("No weather in the scene: defaults apply.");
        if (ui::button("Add weather")) r.emplace<WeatherState>(scene->create_entity("Weather"));
        return;
    }
    WeatherState& s = r.get<WeatherState>(e);
    if (!ui::begin_property_grid("weather")) return;
    std::string_view group;
    for (const WeatherFieldDesc& d : weather::weather_fields()) {
        if (d.group != group) {
            group = d.group;
            ui::prop_separator(sentence_case(group).c_str());
        }
        ImGui::PushID(d.name.data(), d.name.data() + d.name.size());
        draw_field(s, d);
        ImGui::PopID();
    }
    ui::end_property_grid();
    if (ui::button("Reset weather")) s = WeatherState{};
}

} // namespace engine::editor
