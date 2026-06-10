#include "scene_hierarchy.hpp"

#include <string>
#include <vector>

#include <imgui.h>

#include "engine/scene/components.hpp"
#include "engine/scene/scene.hpp"

namespace engine::editor {

namespace {

[[nodiscard]] std::string entity_label(const entt::registry& registry, entt::entity e) {
    if (const auto* name = registry.try_get<scene::Name>(e); name != nullptr &&
                                                            !name->value.empty()) {
        return name->value;
    }
    return "entity " + std::to_string(static_cast<std::uint32_t>(e));
}

// Collects `root` and every descendant (depth-first) into `out`.
void collect_subtree(const entt::registry& registry, entt::entity root,
                     std::vector<entt::entity>& out) {
    out.push_back(root);
    if (const auto* children = registry.try_get<scene::Children>(root)) {
        for (const entt::entity child : children->entities) {
            collect_subtree(registry, child, out);
        }
    }
}

void delete_subtree(scene::Scene& scene, entt::entity root, entt::entity& selected) {
    // Unlink from the parent first so the parent's Children list stays sane,
    // then destroy the whole subtree.
    scene.set_parent(root, entt::null);
    std::vector<entt::entity> doomed;
    collect_subtree(scene.registry(), root, doomed);
    for (const entt::entity e : doomed) {
        if (e == selected) selected = entt::null;
        scene.registry().destroy(e);
    }
}

// Shallow duplicate: clones the common components onto a sibling entity.
// Children are not cloned.
void duplicate_entity(scene::Scene& scene, entt::entity source) {
    entt::registry& r = scene.registry();
    const entt::entity copy = scene.create_entity(entity_label(r, source) + " copy");
    r.get<scene::Transform>(copy) = r.get<scene::Transform>(source);
    if (const auto* c = r.try_get<scene::MeshRenderer>(source)) r.emplace<scene::MeshRenderer>(copy, *c);
    if (const auto* c = r.try_get<scene::PointLight>(source)) r.emplace<scene::PointLight>(copy, *c);
    if (const auto* c = r.try_get<scene::DirectionalLight>(source)) {
        r.emplace<scene::DirectionalLight>(copy, *c);
    }
    if (const auto* c = r.try_get<scene::Camera>(source)) r.emplace<scene::Camera>(copy, *c);
    if (const auto* p = r.try_get<scene::Parent>(source); p != nullptr && p->entity != entt::null) {
        scene.set_parent(copy, p->entity);
    }
}

// Draws one entity row (and its subtree). Returns false if the entity was
// deleted (the caller's child list iteration must stop — it was mutated).
[[nodiscard]] bool draw_entity_node(scene::Scene& scene, entt::entity e,
                                    entt::entity& selected) {
    entt::registry& r = scene.registry();
    const auto* children = r.try_get<scene::Children>(e);
    const bool leaf = children == nullptr || children->entities.empty();

    ImGuiTreeNodeFlags flags = ImGuiTreeNodeFlags_OpenOnArrow |
                               ImGuiTreeNodeFlags_SpanAvailWidth |
                               ImGuiTreeNodeFlags_DefaultOpen;
    if (leaf) flags |= ImGuiTreeNodeFlags_Leaf;
    if (e == selected) flags |= ImGuiTreeNodeFlags_Selected;

    const bool open = ImGui::TreeNodeEx(reinterpret_cast<void*>(static_cast<std::uintptr_t>(e)),
                                        flags, "%s", entity_label(r, e).c_str());
    if (ImGui::IsItemClicked(ImGuiMouseButton_Left) && !ImGui::IsItemToggledOpen()) {
        selected = e;
    }

    bool deleted = false;
    if (ImGui::BeginPopupContextItem()) {
        selected = e;
        if (ImGui::MenuItem("Create child")) {
            const entt::entity child = scene.create_entity("entity");
            scene.set_parent(child, e);
        }
        if (ImGui::MenuItem("Duplicate")) {
            duplicate_entity(scene, e);
        }
        if (ImGui::MenuItem("Delete")) {
            delete_subtree(scene, e, selected);
            deleted = true;
        }
        ImGui::EndPopup();
    }

    if (open) {
        if (!deleted && children != nullptr) {
            // Iterate a copy: deleting/duplicating mutates the Children vector.
            const std::vector<entt::entity> snapshot = children->entities;
            for (const entt::entity child : snapshot) {
                if (!r.valid(child)) continue;
                if (!draw_entity_node(scene, child, selected)) break;
            }
        }
        ImGui::TreePop();
    }
    return !deleted;
}

} // namespace

void draw_scene_hierarchy(scene::Scene& scene, entt::entity& selected) {
    if (!ImGui::Begin("Hierarchy")) {
        ImGui::End();
        return;
    }

    entt::registry& r = scene.registry();
    // Roots: entities with a Transform and no (live) parent. Iterate a snapshot
    // — context-menu actions mutate the registry mid-walk.
    std::vector<entt::entity> roots;
    for (const entt::entity e : r.view<scene::Transform>()) {
        const auto* parent = r.try_get<scene::Parent>(e);
        if (parent == nullptr || parent->entity == entt::null || !r.valid(parent->entity)) {
            roots.push_back(e);
        }
    }
    for (const entt::entity e : roots) {
        if (!r.valid(e)) continue;
        if (!draw_entity_node(scene, e, selected)) break;
    }

    // Context menu on the empty area below the tree.
    if (ImGui::BeginPopupContextWindow("hierarchy_blank",
                                       ImGuiPopupFlags_MouseButtonRight |
                                           ImGuiPopupFlags_NoOpenOverItems)) {
        if (ImGui::MenuItem("Create entity")) {
            (void)scene.create_entity("entity");
        }
        ImGui::EndPopup();
    }
    ImGui::End();
}

} // namespace engine::editor
