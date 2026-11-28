#include "script_inspector.hpp"

#include <algorithm>
#include <cstdio>
#include <string>
#include <vector>

#include <entt/entity/registry.hpp>
#include <imgui.h>

#include "engine/scene/components.hpp"
#include "engine/scene/script_component.hpp"
#include "engine/script/script_runtime.hpp"
#include "engine/script/script_source.hpp"

namespace engine::editor {

namespace {

const ImVec4 k_error(1.0F, 0.4F, 0.35F, 1.0F);
const ImVec4 k_ok(0.45F, 0.85F, 0.45F, 1.0F);

[[nodiscard]] const script::ScriptStatus* find_status(const script::ScriptRuntime* scripts,
                                                      const std::string& path) {
    if (scripts == nullptr) return nullptr;
    for (const script::ScriptStatus& s : scripts->statuses()) {
        if (s.path == path) return &s;
    }
    return nullptr;
}

void draw_path(scene::Script& sc, script::ScriptRuntime* scripts) {
    char buf[scene::k_max_script_path + 1] = {};
    std::snprintf(buf, sizeof(buf), "%s", sc.path.c_str());
    ImGui::SetNextItemWidth(-60.0F);
    if (ImGui::InputText("##path", buf, sizeof(buf), ImGuiInputTextFlags_EnterReturnsTrue) ||
        ImGui::IsItemDeactivatedAfterEdit()) {
        if (scene::validate_script_path(buf)) sc.path = buf;
    }
    if (!sc.path.empty()) {
        if (auto ok = scene::validate_script_path(sc.path); !ok) {
            ImGui::TextColored(k_error, "%s", ok.error().message.c_str());
        }
    }
    ImGui::SameLine();
    if (scripts != nullptr && ImGui::BeginCombo("##pick", "pick", ImGuiComboFlags_NoPreview)) {
        for (const std::string& path : script::list_scripts(scripts->scripts_root())) {
            if (ImGui::Selectable(path.c_str(), path == sc.path)) sc.path = path;
        }
        ImGui::EndCombo();
    }
}

void draw_status(const script::ScriptRuntime* scripts, const std::string& path) {
    const script::ScriptStatus* s = find_status(scripts, path);
    if (s == nullptr) return;
    if (s->state == script::ScriptStatus::State::error) {
        ImGui::PushTextWrapPos(0.0F);
        ImGui::TextColored(k_error, "%s", s->message.c_str());
        ImGui::PopTextWrapPos();
    } else if (scripts->running()) {
        ImGui::TextColored(k_ok, "running (%u instance%s)", s->instances,
                           s->instances == 1 ? "" : "s");
    }
}

// The value widget for one property; true when edited.
bool edit_value(const char* label, scene::ScriptProperty& p) {
    switch (p.type) {
    case scene::ScriptValueType::number:
        return ImGui::DragScalar(label, ImGuiDataType_Double, &p.number, 0.05F);
    case scene::ScriptValueType::boolean: return ImGui::Checkbox(label, &p.boolean);
    case scene::ScriptValueType::string: {
        char buf[512] = {};
        std::snprintf(buf, sizeof(buf), "%s", p.text.c_str());
        if (ImGui::InputText(label, buf, sizeof(buf))) {
            p.text = buf;
            return true;
        }
        return false;
    }
    case scene::ScriptValueType::vec3: return ImGui::DragFloat3(label, &p.vec.x, 0.05F);
    }
    return false;
}

void draw_properties(scene::Script& sc, script::ScriptRuntime* scripts) {
    std::vector<scene::ScriptProperty> declared;
    if (scripts != nullptr && !sc.path.empty()) {
        if (auto d = scripts->declared_properties(sc.path); d) {
            declared = std::move(*d);
        } else {
            ImGui::TextDisabled("properties unavailable: %s", d.error().message.c_str());
        }
    }
    // Declared properties: the override when there is one, else the default.
    for (const scene::ScriptProperty& def : declared) {
        ImGui::PushID(def.name.c_str());
        auto it = std::ranges::find(sc.properties, def.name, &scene::ScriptProperty::name);
        const bool overridden = it != sc.properties.end() && it->type == def.type;
        scene::ScriptProperty value = overridden ? *it : def;
        if (edit_value(def.name.c_str(), value)) {
            if (it != sc.properties.end()) {
                *it = value;
            } else {
                sc.properties.push_back(value);
            }
            ++sc.revision;
        }
        if (overridden) {
            ImGui::SameLine();
            if (ImGui::SmallButton("reset")) {
                sc.properties.erase(it);
                ++sc.revision;
            }
        }
        ImGui::PopID();
    }
    // Overrides the script no longer declares.
    for (auto it = sc.properties.begin(); it != sc.properties.end();) {
        const bool known = std::ranges::any_of(declared, [&](const auto& d) {
            return d.name == it->name && d.type == it->type;
        });
        if (known || declared.empty()) {
            ++it;
            continue;
        }
        ImGui::PushID(it->name.c_str());
        ImGui::TextDisabled("%s (not declared)", it->name.c_str());
        ImGui::SameLine();
        const bool drop = ImGui::SmallButton("remove");
        ImGui::PopID();
        if (drop) {
            it = sc.properties.erase(it);
            ++sc.revision;
        } else {
            ++it;
        }
    }
    // Without a description (no runtime, or a broken script) the overrides
    // stay editable as they are.
    if (declared.empty()) {
        for (scene::ScriptProperty& p : sc.properties) {
            ImGui::PushID(p.name.c_str());
            if (edit_value(p.name.c_str(), p)) ++sc.revision;
            ImGui::PopID();
        }
    }
}

} // namespace

bool draw_script_component(entt::registry& registry, entt::entity entity, scene::Script& sc,
                           script::ScriptRuntime* scripts) {
    const bool open = ImGui::CollapsingHeader("Script", ImGuiTreeNodeFlags_DefaultOpen);
    if (ImGui::BeginPopupContextItem("Script")) {
        const bool remove = ImGui::MenuItem("Remove component");
        ImGui::EndPopup();
        if (remove) {
            registry.remove<scene::Script>(entity);
            return false;
        }
    }
    if (!open) return true;
    draw_script_fields(sc, scripts);
    return true;
}

void draw_script_fields(scene::Script& sc, script::ScriptRuntime* scripts) {
    ImGui::PushID("script");
    draw_path(sc, scripts);
    ImGui::Checkbox("enabled", &sc.enabled);
    draw_status(scripts, sc.path);
    draw_properties(sc, scripts);
    ImGui::PopID();
}

void draw_script_status(const script::ScriptRuntime* scripts) {
    if (scripts == nullptr) return;
    const std::vector<script::ScriptStatus>& all = scripts->statuses();
    std::size_t errors = 0;
    for (const script::ScriptStatus& s : all) errors += s.state == script::ScriptStatus::State::error;
    char header[96];
    std::snprintf(header, sizeof(header), "Scripts (%zu, %zu error%s%s)###scripts", all.size(),
                  errors, errors == 1 ? "" : "s", scripts->running() ? ", running" : "");
    if (errors > 0) ImGui::SetNextItemOpen(true, ImGuiCond_Always);
    if (!ImGui::CollapsingHeader(header)) return;
    for (const script::ScriptStatus& s : all) {
        ImGui::PushID(s.path.c_str());
        const bool failed = s.state == script::ScriptStatus::State::error;
        ImGui::TextColored(failed ? k_error : k_ok, "%-6s", failed ? "error" : "ok");
        ImGui::SameLine();
        ImGui::Text("%s  (%u running, %u stopped, x%u reloads)", s.path.c_str(), s.instances,
                    s.failed, s.reloads);
        if (!s.message.empty()) {
            if (failed) ImGui::SetNextItemOpen(true, ImGuiCond_Once);
            if (ImGui::TreeNode("error")) {
                ImGui::PushTextWrapPos(0.0F);
                ImGui::TextColored(k_error, "%s", s.message.c_str());
                ImGui::PopTextWrapPos();
                if (ImGui::SmallButton("Copy")) ImGui::SetClipboardText(s.message.c_str());
                ImGui::TreePop();
            }
        }
        ImGui::PopID();
    }
}

} // namespace engine::editor
