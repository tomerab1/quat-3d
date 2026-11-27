#include "scene_hierarchy.hpp"

#include <algorithm>
#include <cstdint>
#include <cstring>
#include <string>
#include <vector>

#include <entt/entity/registry.hpp>
#include <imgui.h>
#include <imgui_stdlib.h>

#include "engine/scene/components.hpp"
#include "engine/scene/scene.hpp"
#include "engine/scene/world_clock.hpp"
#include "scene_snapshot.hpp"
#include "shell/scene_actions.hpp"
#include "ui/fonts.hpp"
#include "ui/icons.hpp"
#include "ui/metrics.hpp"
#include "ui/widgets.hpp"

namespace engine::editor {

namespace {

constexpr const char* k_entity_payload = "QUAT_ENTITY";

// Icon tint by category: lights warm, cameras info, audio accent, AI and
// physics success, everything else secondary.
[[nodiscard]] ui::Rgba icon_tint(const entt::registry& r, entt::entity e) {
    const ui::Theme& t = ui::tokens();
    if (r.any_of<scene::DirectionalLight, scene::PointLight>(e)) return t.warning;
    if (r.all_of<scene::Camera>(e)) return t.info;
    if (r.any_of<scene::AudioSource, scene::AudioListener>(e)) return t.accent_text;
    if (r.any_of<scene::BehaviorTree, scene::NavAgent, scene::CharacterController>(e)) {
        return t.success;
    }
    return t.text_secondary;
}

// Context-menu "Create" entries, grouped like the main menu's Create menu.
// Returns the created entity (entt::null if none).
entt::entity create_menu(scene::Scene& scene, entt::entity parent, const glm::vec3& at) {
    entt::entity created = entt::null;
    std::string_view group = "\x01";
    for (const shell::CreateKind& k : shell::create_kinds()) {
        if (k.group != group) {
            if (!k.group.empty()) ImGui::SeparatorText(std::string(k.group).c_str());
            group = k.group;
        }
        if (ImGui::MenuItem(ui::label(k.icon, k.label).c_str())) {
            created = shell::create_entity_of(scene, k, parent, at);
        }
    }
    return created;
}

struct RowContext {
    scene::Scene&     scene;
    entt::entity&     selected;
    HierarchyState&   state;
    const glm::vec3&  spawn_at;
    bool              mutated = false; // the registry changed: stop walking this frame
};

// Inline rename field in place of the label; Enter / focus loss commit,
// Esc cancels (InputText deactivates on Esc, so the key is checked then).
void rename_field(RowContext& ctx, entt::entity e) {
    if (ctx.state.focus_rename) {
        ImGui::SetKeyboardFocusHere();
        ctx.state.focus_rename = false;
    }
    ImGui::SetNextItemWidth(-1.0F);
    const bool enter = ImGui::InputText("##rename", &ctx.state.rename_text,
                                        ImGuiInputTextFlags_EnterReturnsTrue |
                                            ImGuiInputTextFlags_AutoSelectAll);
    if (!enter && !ImGui::IsItemDeactivated()) return;
    if (enter || !ImGui::IsKeyPressed(ImGuiKey_Escape, false)) {
        commit_rename(ctx.scene.registry(), e, ctx.state.rename_text);
    }
    ctx.state.renaming = entt::null;
}

void row_context_menu(RowContext& ctx, entt::entity e) {
    if (!ImGui::BeginPopupContextItem("##row_menu")) return;
    ctx.selected = e;
    if (ImGui::BeginMenu(ui::label(icon::plus, "Create Child").c_str())) {
        const auto* t = ctx.scene.registry().try_get<scene::Transform>(e);
        const glm::vec3 at = t != nullptr ? glm::vec3(t->world[3]) : ctx.spawn_at;
        if (const entt::entity c = create_menu(ctx.scene, e, at); c != entt::null) {
            ctx.selected = c;
            ctx.mutated = true;
        }
        ImGui::EndMenu();
    }
    ImGui::Separator();
    if (ImGui::MenuItem(ui::label(icon::type, "Rename").c_str(), "F2")) {
        begin_rename(ctx.state, ctx.scene, e);
    }
    if (ImGui::MenuItem(ui::label(icon::copy, "Duplicate").c_str(), "Ctrl+D")) {
        if (const entt::entity c = duplicate_entity(ctx.scene, e); c != entt::null) ctx.selected = c;
        ctx.mutated = true;
    }
    if (ctx.scene.registry().all_of<scene::Parent>(e) &&
        ImGui::MenuItem(ui::label(icon::arrow_up, "Unparent").c_str())) {
        (void)shell::reparent_keep_world(ctx.scene, e, entt::null);
        ctx.mutated = true;
    }
    ImGui::Separator();
    ImGui::PushStyleColor(ImGuiCol_Text, ui::to_imvec4(ui::tokens().error));
    if (ImGui::MenuItem(ui::label(icon::trash_2, "Delete").c_str(), "Delete")) {
        shell::delete_entity_tree(ctx.scene, e, ctx.selected);
        ctx.mutated = true;
    }
    ImGui::PopStyleColor();
    ImGui::EndPopup();
}

// Drag a row onto another to re-parent it, keeping its world pose (refused
// when it would create a cycle: Scene::set_parent checks).
void drag_and_drop(RowContext& ctx, entt::entity e, const std::string& label) {
    if (ImGui::BeginDragDropSource()) {
        const auto id = static_cast<std::uint32_t>(e);
        ImGui::SetDragDropPayload(k_entity_payload, &id, sizeof(id));
        ImGui::TextUnformatted(ui::label(icon::move, label).c_str());
        ImGui::EndDragDropSource();
    }
    if (ImGui::BeginDragDropTarget()) {
        if (const ImGuiPayload* p = ImGui::AcceptDragDropPayload(k_entity_payload)) {
            std::uint32_t id = 0;
            std::memcpy(&id, p->Data, sizeof(id));
            const auto dragged = static_cast<entt::entity>(id);
            if (ctx.scene.registry().valid(dragged) && dragged != e) {
                (void)shell::reparent_keep_world(ctx.scene, dragged, e);
                ctx.mutated = true;
            }
        }
        ImGui::EndDragDropTarget();
    }
}

void draw_row(RowContext& ctx, entt::entity e, bool flat) {
    entt::registry& r = ctx.scene.registry();
    const auto* children = r.try_get<scene::Children>(e);
    const bool leaf = flat || children == nullptr || children->entities.empty();
    const std::string label = entity_label(r, e);

    ImGui::PushID(static_cast<int>(static_cast<std::uint32_t>(e)));
    ImGuiTreeNodeFlags flags = ImGuiTreeNodeFlags_OpenOnArrow | ImGuiTreeNodeFlags_OpenOnDoubleClick |
                               ImGuiTreeNodeFlags_SpanFullWidth | ImGuiTreeNodeFlags_DefaultOpen |
                               ImGuiTreeNodeFlags_FramePadding;
    if (leaf) flags |= ImGuiTreeNodeFlags_Leaf | ImGuiTreeNodeFlags_NoTreePushOnOpen;
    if (e == ctx.selected) flags |= ImGuiTreeNodeFlags_Selected;
    const bool renaming = ctx.state.renaming == e;

    // Label: the tree node draws only the arrow; icon + name are drawn after
    // it so the icon can carry its own tint.
    const bool open = ImGui::TreeNodeEx("##node", flags);
    const bool toggled = ImGui::IsItemToggledOpen();
    if (ImGui::IsItemClicked(ImGuiMouseButton_Left) && !toggled) ctx.selected = e;
    if (ImGui::IsItemHovered() && ImGui::IsMouseDoubleClicked(ImGuiMouseButton_Left) && leaf) {
        begin_rename(ctx.state, ctx.scene, e);
    }
    row_context_menu(ctx, e);
    if (!ctx.mutated) drag_and_drop(ctx, e, label);

    ImGui::SameLine(0.0F, 0.0F);
    const char* glyph = ui::ic(entity_type_icon(r, e));
    if (glyph[0] != '\0') {
        ui::text_colored(icon_tint(r, e), glyph);
        ImGui::SameLine(0.0F, ui::scaled(ui::Spacing::xs));
    }
    if (renaming && !ctx.mutated) {
        rename_field(ctx, e);
    } else {
        ImGui::TextUnformatted(label.c_str());
    }

    if (open && !leaf) {
        if (!ctx.mutated) {
            // Iterate a copy: actions in child rows mutate the Children list.
            const std::vector<entt::entity> snapshot = children->entities;
            for (const entt::entity child : snapshot) {
                if (ctx.mutated) break;
                if (r.valid(child)) draw_row(ctx, child, false);
            }
        }
        ImGui::TreePop();
    }
    ImGui::PopID();
}

} // namespace

const char* entity_type_icon(const entt::registry& r, entt::entity e) {
    if (r.all_of<scene::Camera>(e)) return icon::camera;
    if (r.all_of<scene::DirectionalLight>(e)) return icon::sun;
    if (r.all_of<scene::PointLight>(e)) return icon::lightbulb;
    if (r.all_of<scene::Terrain>(e)) return icon::mountain;
    if (r.all_of<scene::Grass>(e)) return icon::sprout;
    if (r.all_of<scene::WorldClock>(e)) return icon::clock_3;
    if (r.all_of<scene::AudioSource>(e)) return icon::volume_2;
    if (r.all_of<scene::AudioListener>(e)) return icon::ear;
    if (r.all_of<scene::CharacterController>(e)) return icon::person_standing;
    if (r.all_of<scene::BehaviorTree>(e)) return icon::workflow;
    if (r.all_of<scene::NavAgent>(e)) return icon::footprints;
    if (r.all_of<scene::Scatter>(e)) return icon::trees;
    if (r.all_of<scene::InstancedMesh>(e)) return icon::boxes;
    if (r.all_of<scene::SkinnedMesh>(e)) return icon::person_standing;
    if (r.all_of<scene::Primitive>(e)) return icon::shapes;
    if (r.all_of<scene::MeshRenderer>(e)) return icon::box;
    if (r.any_of<scene::Collider, scene::RigidBody>(e)) return icon::shield;
    if (const auto* c = r.try_get<scene::Children>(e); c != nullptr && !c->entities.empty()) {
        return icon::folder;
    }
    return icon::circle_dot;
}

std::string entity_label(const entt::registry& registry, entt::entity e) {
    if (const auto* name = registry.try_get<scene::Name>(e); name != nullptr &&
                                                            !name->value.empty()) {
        return name->value;
    }
    return "entity " + std::to_string(static_cast<std::uint32_t>(e));
}

void commit_rename(entt::registry& registry, entt::entity e, std::string_view text) {
    if (!registry.valid(e)) return;
    registry.get_or_emplace<scene::Name>(e).value = std::string(text);
}

std::vector<entt::entity> filter_entities(const entt::registry& registry, std::string_view filter) {
    std::vector<entt::entity> out;
    for (const entt::entity e : registry.view<scene::Transform>()) {
        if (ui::matches_filter(entity_label(registry, e), filter)) out.push_back(e);
    }
    return out;
}

void begin_rename(HierarchyState& state, const scene::Scene& scene, entt::entity e) {
    if (!scene.registry().valid(e)) return;
    state.renaming = e;
    const auto* name = scene.registry().try_get<scene::Name>(e);
    state.rename_text = name != nullptr ? name->value : std::string();
    state.focus_rename = true;
}

void draw_scene_hierarchy(scene::Scene& scene, entt::entity& selected, HierarchyState& state,
                          const glm::vec3& spawn_at, const char* window_name, bool* open) {
    if (open != nullptr && !*open) return;
    if (!ImGui::Begin(window_name, open)) {
        ImGui::End();
        return;
    }
    entt::registry& r = scene.registry();
    if (state.renaming != entt::null && !r.valid(state.renaming)) state.renaming = entt::null;

    // Header: search + create.
    const float add_w = ImGui::GetFrameHeight();
    (void)ui::search_field("hierarchy_search", state.filter, "Search entities",
                           ImGui::GetContentRegionAvail().x - add_w - ImGui::GetStyle().ItemSpacing.x);
    ImGui::SameLine();
    if (ui::icon_button("add", icon::plus, "Create an entity")) ImGui::OpenPopup("##create");
    if (ImGui::BeginPopup("##create")) {
        if (const entt::entity c = create_menu(scene, entt::null, spawn_at); c != entt::null) {
            selected = c;
        }
        ImGui::EndPopup();
    }

    // Roots: entities with a Transform and no (live) parent.
    std::vector<entt::entity> roots;
    std::size_t total = 0;
    for (const entt::entity e : r.view<scene::Transform>()) {
        ++total;
        const auto* parent = r.try_get<scene::Parent>(e);
        if (parent == nullptr || parent->entity == entt::null || !r.valid(parent->entity)) {
            roots.push_back(e);
        }
    }

    ImGui::BeginChild("##tree", ImVec2(0.0F, 0.0F), ImGuiChildFlags_None);
    RowContext ctx{scene, selected, state, spawn_at};
    ImGui::PushStyleVar(ImGuiStyleVar_FramePadding, ImVec2(ui::scaled(ui::Spacing::xxs), ui::scaled(3.0F)));
    ImGui::PushStyleVar(ImGuiStyleVar_ItemSpacing, ImVec2(ui::scaled(ui::Spacing::xs), 0.0F));
    if (total == 0) {
        ImGui::PopStyleVar(2);
        if (ui::empty_state(icon::boxes, "The scene is empty",
                            "Create an entity, or drag a glTF model from Assets into the viewport.",
                            "Create Entity")) {
            const entt::entity c = shell::create_entity_of(scene, shell::create_kinds()[0],
                                                           entt::null, spawn_at);
            if (c != entt::null) selected = c;
        }
        ImGui::PushStyleVar(ImGuiStyleVar_FramePadding, ImVec2(0.0F, 0.0F));
        ImGui::PushStyleVar(ImGuiStyleVar_ItemSpacing, ImVec2(0.0F, 0.0F));
    } else if (!state.filter.empty()) {
        // Filtered: a flat list of every matching entity.
        const std::vector<entt::entity> matches = filter_entities(r, state.filter);
        for (const entt::entity e : matches) {
            if (ctx.mutated) break;
            draw_row(ctx, e, /*flat=*/true);
        }
        if (matches.empty()) {
            ImGui::PopStyleVar(2);
            (void)ui::empty_state(icon::search, "No matching entities",
                                  "Names are matched case-insensitively.");
            ImGui::PushStyleVar(ImGuiStyleVar_FramePadding, ImVec2(0.0F, 0.0F));
            ImGui::PushStyleVar(ImGuiStyleVar_ItemSpacing, ImVec2(0.0F, 0.0F));
        }
    } else {
        for (const entt::entity e : roots) {
            if (ctx.mutated) break;
            if (r.valid(e)) draw_row(ctx, e, false);
        }
    }
    ImGui::PopStyleVar(2);

    // The blank area below the rows: drop to un-parent, right-click to create.
    const ImVec2 rest = ImGui::GetContentRegionAvail();
    if (rest.y > 1.0F) {
        ImGui::InvisibleButton("##blank", ImVec2(std::max(rest.x, 1.0F), rest.y));
        if (ImGui::IsItemClicked(ImGuiMouseButton_Left)) selected = entt::null;
        if (ImGui::BeginDragDropTarget()) {
            if (const ImGuiPayload* p = ImGui::AcceptDragDropPayload(k_entity_payload)) {
                std::uint32_t id = 0;
                std::memcpy(&id, p->Data, sizeof(id));
                const auto dragged = static_cast<entt::entity>(id);
                if (r.valid(dragged)) (void)shell::reparent_keep_world(scene, dragged, entt::null);
            }
            ImGui::EndDragDropTarget();
        }
        if (ImGui::BeginPopupContextItem("##blank_menu")) {
            if (const entt::entity c = create_menu(scene, entt::null, spawn_at); c != entt::null) {
                selected = c;
            }
            ImGui::EndPopup();
        }
    }
    ImGui::EndChild();
    ImGui::End();
}

} // namespace engine::editor
