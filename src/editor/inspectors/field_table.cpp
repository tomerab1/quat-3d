// Inspector sections for field-table components (ROADMAP G17.1): game module
// components (engine::game::ComponentFields) get rows from their field
// tables, since games never include ImGui. One section per such component,
// after the engine's own, with the codec's reset / copy / paste / remove.

#include <cstdint>
#include <string>
#include <utility>
#include <vector>

#include "inspect.hpp"

#include "engine/scene/component_registry.hpp"
#include "engine/scene/field_table.hpp"

namespace engine::editor {

namespace {

// "coin_spin" -> "Coin Spin".
[[nodiscard]] std::string title_of(std::string_view name) {
    std::string out;
    bool start = true;
    for (const char c : name) {
        if (c == '_') {
            out += ' ';
            start = true;
            continue;
        }
        out += start && c >= 'a' && c <= 'z' ? static_cast<char>(c - 'a' + 'A') : c;
        start = false;
    }
    return out;
}

[[nodiscard]] ui::DragSpec drag_of(const scene::FieldDesc& d) {
    ui::DragSpec spec;
    const bool bounded = d.min > -1.0e29 && d.max < 1.0e29;
    if (bounded) {
        spec.min = static_cast<float>(d.min);
        spec.max = static_cast<float>(d.max);
        spec.clamp = true;
        spec.speed = static_cast<float>((d.max - d.min) / 200.0);
    }
    return spec;
}

void draw_field(const scene::FieldDesc& d, void* p) {
    const std::string label = title_of(d.name);
    const char* tooltip = d.description.empty() ? nullptr : d.description.c_str();
    switch (d.kind) {
    case scene::FieldKind::boolean:
        (void)ui::prop_bool(label.c_str(), *static_cast<bool*>(p),
                            {.tooltip = tooltip, .read_only = d.read_only});
        break;
    case scene::FieldKind::integer: {
        ui::IntSpec spec;
        if (d.min > -2.0e9 && d.max < 2.0e9) {
            spec.min = static_cast<int>(d.min);
            spec.max = static_cast<int>(d.max);
        }
        (void)ui::prop_int(label.c_str(), *static_cast<std::int32_t*>(p), spec,
                           {.tooltip = tooltip, .read_only = d.read_only});
        break;
    }
    case scene::FieldKind::number:
        (void)ui::prop_float(label.c_str(), *static_cast<float*>(p), drag_of(d),
                             {.tooltip = tooltip, .read_only = d.read_only});
        break;
    case scene::FieldKind::vec2:
        (void)ui::prop_vec2(label.c_str(), *static_cast<glm::vec2*>(p), drag_of(d),
                            {.tooltip = tooltip, .read_only = d.read_only});
        break;
    case scene::FieldKind::vec3:
        (void)ui::prop_vec3(label.c_str(), *static_cast<glm::vec3*>(p), drag_of(d),
                            {.tooltip = tooltip, .read_only = d.read_only});
        break;
    case scene::FieldKind::color3:
        (void)ui::prop_color3(label.c_str(), *static_cast<glm::vec3*>(p),
                              {.tooltip = tooltip, .read_only = d.read_only});
        break;
    case scene::FieldKind::color4:
        (void)ui::prop_color4(label.c_str(), *static_cast<glm::vec4*>(p),
                              {.tooltip = tooltip, .read_only = d.read_only});
        break;
    case scene::FieldKind::string: {
        auto& s = *static_cast<std::string*>(p);
        std::string edited = s;
        if (ui::prop_text(label.c_str(), edited, {.tooltip = tooltip, .read_only = d.read_only}, true) &&
            edited.size() <= d.max_length) {
            s = std::move(edited); // the codec's length limit holds in the editor too
        }
        break;
    }
    case scene::FieldKind::choice: {
        std::vector<ui::EnumItem> items;
        for (std::size_t i = 0; i < d.choices.size(); ++i) {
            items.push_back({.value = static_cast<int>(i), .label = d.choices[i].c_str(), .tooltip = nullptr});
        }
        (void)ui::prop_enum(label.c_str(), *static_cast<std::int32_t*>(p), items,
                            {.tooltip = tooltip, .read_only = d.read_only});
        break;
    }
    }
}

void draw_field_components(panels::InspectorState& state, entt::registry& r, entt::entity e) {
    const scene::ComponentRegistry& components = scene::component_registry();
    for (std::size_t i = 0; i < components.size(); ++i) {
        const scene::ComponentCodec& codec = components.at(i);
        if (!codec.fields || codec.fields->component(r, e) == nullptr) continue;
        const std::string title = title_of(codec.name);
        const panels::SectionResult s = panels::component_section(
            r, e, state, codec.name.c_str(), icon::box, title.c_str(), nullptr, codec.remove != nullptr);
        if (s.removed) {
            (void)panels::remove_section(r, e, codec.name.c_str());
            continue;
        }
        void* component = codec.fields->component(r, e); // a reset / paste may have replaced it
        if (!s.open || component == nullptr) continue;
        ImGui::PushID(codec.name.c_str());
        ImGui::Indent(ui::scaled(ui::Spacing::xxs));
        if (ui::begin_property_grid("##grid")) {
            for (std::size_t f = 0; f < codec.fields->fields.size(); ++f) {
                draw_field(codec.fields->fields[f], codec.fields->address[f](component));
            }
            ui::end_property_grid();
        }
        ImGui::Unindent(ui::scaled(ui::Spacing::xxs));
        ImGui::Dummy(ImVec2(0.0F, ui::scaled(ui::Spacing::xxs)));
        ImGui::PopID();
    }
}

} // namespace

void register_field_table_inspector(InspectorRegistry& registry) {
    registry.add({.name = "field_table",
                  .order = 100'000, // after every engine section
                  .draw = &draw_field_components});
}

} // namespace engine::editor
