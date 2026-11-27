#include "weather_panel.hpp"

#include <string>
#include <string_view>

#include <entt/entity/registry.hpp>
#include <imgui.h>

#include "engine/scene/scene.hpp"
#include "engine/weather/weather_state.hpp"

namespace engine::editor {

namespace {

using weather::WeatherFieldDesc;
using weather::WeatherFieldRef;
using weather::WeatherState;

void tooltip(std::string_view hint) {
    if (!hint.empty()) ImGui::SetItemTooltip("%.*s", static_cast<int>(hint.size()), hint.data());
}

[[nodiscard]] std::string slider_format(const WeatherFieldDesc& d) {
    const float span = d.max - d.min;
    std::string f = span <= 2.0F ? "%.2f" : span <= 20.0F ? "%.1f" : "%.0f";
    if (!d.unit.empty()) f += " " + std::string(d.unit);
    return f;
}

void draw_field(WeatherState& s, const WeatherFieldDesc& d) {
    const WeatherFieldRef r = d.ref(s);
    const std::string label(d.label);
    if (r.b != nullptr) {
        ImGui::Checkbox(label.c_str(), r.b);
    } else if (r.p != nullptr) {
        int current = static_cast<int>(*r.p);
        const char* items[] = {"rain", "snow"};
        if (ImGui::Combo(label.c_str(), &current, items, 2)) {
            *r.p = static_cast<weather::PrecipitationType>(current);
        }
    } else {
        ImGui::SliderFloat(label.c_str(), r.f, d.min, d.max, slider_format(d).c_str(),
                           ImGuiSliderFlags_AlwaysClamp);
    }
    tooltip(d.hint);
}

} // namespace

void draw_weather_section(scene::Scene* scene) {
    if (scene == nullptr) return;
    entt::registry& r = scene->registry();
    const entt::entity e = weather::find_weather(r);
    if (e == entt::null) {
        ImGui::TextUnformatted("(no weather in the scene: defaults apply)");
        if (ImGui::SmallButton("add weather")) {
            r.emplace<WeatherState>(scene->create_entity("Weather"));
        }
        return;
    }
    WeatherState& s = r.get<WeatherState>(e);
    std::string_view group;
    for (const WeatherFieldDesc& d : weather::weather_fields()) {
        if (d.group != group) {
            group = d.group;
            ImGui::SeparatorText(std::string(group).c_str());
        }
        ImGui::PushID(d.name.data(), d.name.data() + d.name.size());
        draw_field(s, d);
        ImGui::PopID();
    }
    if (ImGui::SmallButton("reset weather")) s = WeatherState{};
}

} // namespace engine::editor
