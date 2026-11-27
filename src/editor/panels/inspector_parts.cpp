#include "inspector_parts.hpp"

#include <algorithm>
#include <cctype>
#include <cstdint>
#include <string>
#include <vector>

#include <entt/entity/registry.hpp>
#include <glm/glm.hpp>
#include <imgui.h>
#include <imgui_stdlib.h>
#include <nlohmann/json.hpp>

#include "../scene_hierarchy.hpp"
#include "../ui/fonts.hpp"
#include "../ui/icons.hpp"
#include "../ui/metrics.hpp"
#include "../ui/toasts.hpp"
#include "../ui/widgets.hpp"
#include "engine/scene/component_json.hpp"
#include "engine/scene/components.hpp"
#include "engine/scene/scene.hpp"

namespace engine::editor::panels {

namespace {

struct CodecIcon {
    std::string_view codec;
    const char*      icon;
};
constexpr CodecIcon k_codec_icons[] = {
    {"transform", icon::move},           {"primitive", icon::shapes},
    {"instanced_mesh", icon::boxes},     {"scatter", icon::trees},
    {"point_light", icon::lightbulb},    {"dir_light", icon::sun},
    {"camera", icon::camera},            {"collider", icon::shield},
    {"rigidbody", icon::atom},           {"character", icon::person_standing},
    {"nav_agent", icon::footprints},     {"behavior", icon::workflow},
    {"patrol", icon::route},             {"terrain", icon::mountain},
    {"grass", icon::sprout},             {"net_replicated", icon::network},
    {"audio_source", icon::volume_2},    {"audio_listener", icon::ear},
    {"animator", icon::clapperboard},    {"mesh", icon::box},
};

void apply_action(entt::registry& r, entt::entity e, InspectorState& state, const char* codec,
                  ui::HeaderAction action) {
    using ui::HeaderAction;
    if (action == HeaderAction::none || action == HeaderAction::remove || codec == nullptr) return;
    const std::string name(codec);
    switch (action) {
    case HeaderAction::reset:
        if (name == "transform") {
            r.get<scene::Transform>(e).local = glm::mat4(1.0F);
        } else {
            (void)scene::remove_component(r, e, name);
            if (auto ok = scene::apply_component(r, e, name, nlohmann::json::object(), false); !ok) {
                ui::notify(ui::ToastKind::error, "Reset failed: " + ok.error().message);
            }
        }
        break;
    case HeaderAction::copy: {
        const std::string only[] = {name};
        scene::EncodeOptions opts;
        opts.only = only;
        const nlohmann::json j = scene::encode_components(r, e, opts);
        if (const auto it = j.find(name); it != j.end()) {
            state.clipboard_type = name;
            state.clipboard_json = it->dump(2, ' ', false, nlohmann::json::error_handler_t::replace);
            ImGui::SetClipboardText(state.clipboard_json.c_str());
            ui::notify(ui::ToastKind::info, "Copied " + component_title(name) + " values", 2.0F);
        }
        break;
    }
    case HeaderAction::paste: {
        const nlohmann::json j =
            nlohmann::json::parse(state.clipboard_json, nullptr, /*allow_exceptions=*/false);
        if (j.is_discarded()) break;
        if (auto ok = scene::apply_component(r, e, name, j, /*strict=*/false); !ok) {
            ui::notify(ui::ToastKind::error, "Paste failed: " + ok.error().message);
        }
        break;
    }
    case HeaderAction::none:
    case HeaderAction::remove:
        break;
    }
}

} // namespace

const char* component_icon(std::string_view codec) {
    for (const CodecIcon& c : k_codec_icons) {
        if (c.codec == codec) return c.icon;
    }
    return icon::box;
}

std::string component_title(std::string_view codec) {
    if (codec == "dir_light") return "Directional Light";
    if (codec == "rigidbody") return "Rigid Body";
    if (codec == "behavior") return "Behavior Tree";
    if (codec == "patrol") return "Patrol Route";
    if (codec == "character") return "Character Controller";
    std::string out;
    bool upper = true;
    for (const char c : codec) {
        if (c == '_') {
            out += ' ';
            upper = true;
            continue;
        }
        out += upper ? static_cast<char>(std::toupper(static_cast<unsigned char>(c))) : c;
        upper = false;
    }
    return out;
}

SectionResult component_section(entt::registry& registry, entt::entity entity,
                                InspectorState& state, const char* codec, const char* icon_glyph,
                                const char* title, bool* enabled, bool removable) {
    const scene::ComponentInfo* info =
        codec != nullptr ? scene::find_component_info(codec) : nullptr;
    const ui::ComponentHeader h = ui::component_header({
        .id = title,
        .icon = icon_glyph,
        .title = title,
        .enabled = enabled,
        .can_reset = codec != nullptr && (info == nullptr || info->writable),
        .can_copy = codec != nullptr,
        .can_paste = codec != nullptr && state.clipboard_type == codec &&
                     (info == nullptr || info->writable),
        .removable = removable,
        .tooltip = info != nullptr ? info->summary.data() : nullptr,
    });
    SectionResult out;
    out.open = h.open;
    if (h.action == ui::HeaderAction::remove) {
        out.removed = true;
        return out;
    }
    apply_action(registry, entity, state, codec, h.action);
    return out;
}

void entity_header(scene::Scene& scene, entt::entity entity) {
    entt::registry& r = scene.registry();
    const ui::Theme& t = ui::tokens();
    ImGui::AlignTextToFramePadding();
    {
        ui::FontScope big(ui::FontRole::icon_large);
        ui::text_colored(t.accent_text, ui::ic(entity_type_icon(r, entity)));
    }
    ImGui::SameLine();
    auto* name = r.try_get<scene::Name>(entity);
    char id[32];
    std::snprintf(id, sizeof(id), "#%u", static_cast<std::uint32_t>(entity));
    const float id_w = ImGui::CalcTextSize(id).x + ui::scaled(ui::Spacing::xs);
    if (name != nullptr) {
        ui::FontScope heading(ui::FontRole::heading);
        ImGui::SetNextItemWidth(ImGui::GetContentRegionAvail().x - id_w);
        ImGui::InputTextWithHint("##entity_name", "Entity name", &name->value);
        ui::item_tooltip("Entity name (F2 renames it in the Hierarchy too)");
    }
    ImGui::SameLine();
    ImGui::AlignTextToFramePadding();
    {
        ui::FontScope mono(ui::FontRole::mono);
        ui::text_colored(t.text_disabled, id);
    }
    ui::item_tooltip("Entity id (runtime only; changes between sessions)");
    ImGui::Spacing();
}

void add_component_button(entt::registry& registry, entt::entity entity, InspectorState& state) {
    ImGui::Spacing();
    const float w = std::min(ImGui::GetContentRegionAvail().x, ui::scaled(260.0F));
    ImGui::SetCursorPosX(ImGui::GetCursorPosX() + (ImGui::GetContentRegionAvail().x - w) * 0.5F);
    if (ui::button(ui::label(icon::plus, "Add Component").c_str(), ui::ButtonKind::primary,
                   ImVec2(w, 0.0F))) {
        state.add_filter.clear();
        ImGui::OpenPopup("##add_component");
    }
    ImGui::SetNextWindowSizeConstraints(ImVec2(ui::scaled(300.0F), 0.0F),
                                        ImVec2(ui::scaled(420.0F), ui::scaled(420.0F)));
    if (!ImGui::BeginPopup("##add_component")) return;
    if (ImGui::IsWindowAppearing()) ImGui::SetKeyboardFocusHere();
    (void)ui::search_field("add_filter", state.add_filter, "Search components");
    const std::vector<std::string_view> present = scene::component_names(registry, entity);
    int shown = 0;
    ImGui::BeginChild("##components", ImVec2(0.0F, ui::scaled(300.0F)));
    for (const scene::ComponentInfo& c : scene::component_infos()) {
        if (!c.addable) continue;
        if (std::find(present.begin(), present.end(), c.name) != present.end()) continue;
        const std::string title = component_title(c.name);
        if (!ui::matches_filter(title, state.add_filter) &&
            !ui::matches_filter(c.summary, state.add_filter)) {
            continue;
        }
        ++shown;
        ImGui::PushID(c.name.data(), c.name.data() + c.name.size());
        const ImVec2 p = ImGui::GetCursorScreenPos();
        const float row_h = ImGui::GetTextLineHeightWithSpacing() * 2.0F + ui::scaled(4.0F);
        if (ImGui::Selectable("##add", false, ImGuiSelectableFlags_None,
                              ImVec2(0.0F, row_h))) {
            if (auto ok = scene::apply_component(registry, entity, c.name,
                                                 nlohmann::json::object(), false);
                !ok) {
                ui::notify(ui::ToastKind::error, "Could not add " + title + ": " + ok.error().message);
            }
            ImGui::CloseCurrentPopup();
        }
        ImDrawList* dl = ImGui::GetWindowDrawList();
        const ui::Theme& t = ui::tokens();
        const float x = p.x + ui::scaled(ui::Spacing::xs);
        dl->AddText(ImVec2(x, p.y + ui::scaled(3.0F)), ui::to_u32(t.accent_text),
                    ui::ic(component_icon(c.name)));
        const float tx = x + ImGui::GetFontSize() + ui::scaled(ui::Spacing::xs);
        dl->AddText(ImVec2(tx, p.y + ui::scaled(3.0F)), ui::to_u32(t.text), title.c_str());
        const std::string summary(c.summary);
        dl->PushClipRect(p, ImVec2(p.x + ImGui::GetContentRegionAvail().x, p.y + row_h), true);
        dl->AddText(ImVec2(tx, p.y + ui::scaled(3.0F) + ImGui::GetTextLineHeightWithSpacing()),
                    ui::to_u32(t.text_secondary), summary.c_str());
        dl->PopClipRect();
        ui::item_tooltip(summary.c_str());
        ImGui::PopID();
    }
    if (shown == 0) {
        (void)ui::empty_state(icon::search, "Nothing to add",
                              state.add_filter.empty() ? "The entity has every component."
                                                       : "No component matches the search.");
    }
    ImGui::EndChild();
    ImGui::EndPopup();
}

} // namespace engine::editor::panels
