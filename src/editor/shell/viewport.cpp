// Viewport panel (ROADMAP E8.2): the scene's offscreen image as a dock
// panel, with an overlay toolbar (overlays / debug views, statistics,
// camera) drawn over it, the transform gizmo (tool, space and snapping from
// the main toolbar) and click picking. Dropping a glTF from the asset
// browser instantiates it.

#include <algorithm>
#include <cstdio>
#include <string>

#include <glm/glm.hpp>
#include <imgui.h>

#include <ImGuizmo.h> // must follow imgui.h

#include "../picking.hpp"
#include "../time_of_day_panel.hpp"
#include "../ui/fonts.hpp"
#include "../ui/icons.hpp"
#include "../ui/metrics.hpp"
#include "../ui/widgets.hpp"
#include "editor_shell.hpp"
#include "engine/renderer/imgui_pass.hpp" // viewport_texture_id
#include "engine/scene/components.hpp"
#include "engine/scene/scene.hpp"

namespace engine::editor::shell {

namespace {

[[nodiscard]] bool is_gltf(std::string_view path) {
    const auto ends = [&](std::string_view ext) {
        return path.size() >= ext.size() && path.substr(path.size() - ext.size()) == ext;
    };
    return ends(".gltf") || ends(".glb");
}

// The overlay toolbar's screen rect (top-left corner of the viewport).
struct Rect {
    ImVec2 min;
    ImVec2 max;
    [[nodiscard]] bool contains(const ImVec2& p) const {
        return p.x >= min.x && p.y >= min.y && p.x < max.x && p.y < max.y;
    }
};
[[nodiscard]] Rect overlay_rect(const ImVec2& viewport_min) {
    const float pad = ui::scaled(ui::Spacing::xs);
    const float h = ImGui::GetFrameHeight();
    const ImVec2 o(viewport_min.x + pad, viewport_min.y + pad);
    return {o, ImVec2(o.x + h * 2.0F + ui::scaled(10.0F), o.y + h + ui::scaled(8.0F))};
}

// ImGuizmo manipulator for the selected entity's Transform. Operates on the
// WORLD matrix, then converts back to local through the parent's world.
void gizmo(EditorShell& shell, const ImVec2& min, const ImVec2& max) {
    const EditorContext& ctx = *shell.ctx;
    entt::registry& r = ctx.scene->registry();
    if (!shell.has_selection()) return;
    auto* transform = r.try_get<scene::Transform>(shell.selected);
    if (transform == nullptr || clock_only_entity(r, shell.selected)) return; // nothing to move

    ImGuizmo::SetOrthographic(false);
    ImGuizmo::SetDrawlist(ImGui::GetWindowDrawList());
    ImGuizmo::SetRect(min.x, min.y, max.x - min.x, max.y - min.y);
    const ImGuizmo::OPERATION op = shell.gizmo_op == GizmoOp::translate ? ImGuizmo::TRANSLATE
                                   : shell.gizmo_op == GizmoOp::rotate  ? ImGuizmo::ROTATE
                                                                        : ImGuizmo::SCALE;
    // Scale always manipulates the object's own axes.
    const ImGuizmo::MODE mode = shell.gizmo_space == GizmoSpace::local || op == ImGuizmo::SCALE
                                    ? ImGuizmo::LOCAL
                                    : ImGuizmo::WORLD;
    float snap[3] = {0.0F, 0.0F, 0.0F};
    const bool snapping = shell.snap.enabled != ImGui::GetIO().KeyCtrl; // Ctrl inverts
    if (snapping) {
        const float step = shell.gizmo_op == GizmoOp::translate ? shell.snap.translate
                           : shell.gizmo_op == GizmoOp::rotate  ? shell.snap.rotate_deg
                                                                : shell.snap.scale;
        snap[0] = snap[1] = snap[2] = step;
    }
    glm::mat4 world = transform->world;
    if (ImGuizmo::Manipulate(&ctx.view[0][0], &ctx.proj_no_flip[0][0], op, mode, &world[0][0],
                             nullptr, snapping ? snap : nullptr)) {
        glm::mat4 parent_world(1.0F);
        if (const auto* parent = r.try_get<scene::Parent>(shell.selected);
            parent != nullptr && parent->entity != entt::null && r.valid(parent->entity)) {
            if (const auto* pt = r.try_get<scene::Transform>(parent->entity)) {
                parent_world = pt->world;
            }
        }
        transform->local = glm::inverse(parent_world) * world;
        transform->world = world; // keep overlays in sync until the next tick
    }
}

// Left-click picking: ray through the clicked pixel against every mesh's
// world-space-transformed local AABB; the nearest hit becomes the selection.
// Ctrl/Shift+click raise ground-click commands instead (walk / waypoint).
void picking(EditorShell& shell, const ImVec2& min, const ImVec2& max) {
    const EditorContext& ctx = *shell.ctx;
    if (!ImGui::IsItemClicked(ImGuiMouseButton_Left)) return;
    if (ImGuizmo::IsOver() || ImGuizmo::IsUsing()) return;
    const ImVec2 mouse = ImGui::GetMousePos();
    if (overlay_rect(min).contains(mouse)) return; // the overlay toolbar's click
    const float w = std::max(max.x - min.x, 1.0F);
    const float h = std::max(max.y - min.y, 1.0F);
    // The flipped view-proj maps NDC y downward, matching screen coordinates.
    const glm::vec2 ndc((mouse.x - min.x) / w * 2.0F - 1.0F, (mouse.y - min.y) / h * 2.0F - 1.0F);
    const glm::mat4 inv_vp = glm::inverse(ctx.view_proj);
    const glm::vec4 near_h = inv_vp * glm::vec4(ndc, 0.0F, 1.0F);
    const glm::vec4 far_h = inv_vp * glm::vec4(ndc, 1.0F, 1.0F);
    const glm::vec3 origin = glm::vec3(near_h) / near_h.w;
    const glm::vec3 dir = glm::normalize(glm::vec3(far_h) / far_h.w - origin);

    const ImGuiIO& io = ImGui::GetIO();
    if ((io.KeyCtrl || io.KeyShift) && ctx.ground_click != nullptr) {
        EditorContext::GroundClick click;
        click.origin = origin;
        click.dir = dir;
        click.add_waypoint = io.KeyShift;
        *ctx.ground_click = click;
        return;
    }
    // Clicking empty space clears the selection (every DCC tool does).
    shell.selected = pick_entity(ctx.scene->registry(), origin, dir);
}

// Translucent backdrop behind overlay controls so they read on any scene.
void overlay_backdrop(const ImVec2& min, const ImVec2& max) {
    ImGui::GetWindowDrawList()->AddRectFilled(
        min, max, ui::to_u32(ui::with_alpha(ui::tokens().bg0, 0.78F)), ui::scaled(ui::Radius::lg));
}

void overlay_toolbar(EditorShell& shell, const ImVec2& min) {
    const Rect r = overlay_rect(min);
    overlay_backdrop(r.min, r.max);
    ImGui::SetCursorScreenPos(ImVec2(r.min.x + ui::scaled(4.0F), r.min.y + ui::scaled(4.0F)));
    ui::begin_toolbar();
    if (ui::icon_button("overlays", icon::eye, "Overlays and debug views")) {
        ImGui::OpenPopup("##overlays");
    }
    ImGui::SameLine();
    if (ui::toggle_button("stats", icon::activity, shell.show_viewport_stats,
                          "Show frame statistics")) {
        shell.show_viewport_stats = !shell.show_viewport_stats;
    }
    ui::end_toolbar();
    if (ImGui::BeginPopup("##overlays")) {
        PhysicsDebugState& p = shell.physics_state;
        ImGui::SeparatorText("Physics");
        ImGui::MenuItem("Colliders", nullptr, &p.enabled);
        ImGui::BeginDisabled(!p.enabled);
        ImGui::MenuItem("    Static", nullptr, &p.show_static);
        ImGui::MenuItem("    Dynamic / kinematic", nullptr, &p.show_dynamic);
        ImGui::MenuItem("    Sensors", nullptr, &p.show_sensors);
        ImGui::EndDisabled();
        ImGui::SeparatorText("Navigation");
        ImGui::MenuItem("Navmesh", nullptr, &p.show_navmesh);
        ImGui::SeparatorText("Animation");
        ImGui::MenuItem("Skeleton joints", nullptr, &shell.anim_state.show_joints);
        ImGui::EndPopup();
    }
}

// Frame statistics in the top-right corner.
void stats_overlay(EditorShell& shell, const ImVec2& min, const ImVec2& max) {
    if (!shell.show_viewport_stats) return;
    const EditorContext& ctx = *shell.ctx;
    const ui::Theme& t = ui::tokens();
    ui::FontScope mono(ui::FontRole::mono);
    const float fps = ImGui::GetIO().Framerate;
    char lines[4][64];
    std::snprintf(lines[0], sizeof(lines[0]), "%6.1f fps", static_cast<double>(fps));
    std::snprintf(lines[1], sizeof(lines[1]), "%6.2f ms cpu", static_cast<double>(ctx.frame_ms));
    std::snprintf(lines[2], sizeof(lines[2]), "%6d draws", ctx.draw_count);
    std::snprintf(lines[3], sizeof(lines[3]), "%u x %u", shell.viewport_width,
                  shell.viewport_height);
    float w = 0.0F;
    for (const auto& l : lines) w = std::max(w, ImGui::CalcTextSize(l).x);
    const float pad = ui::scaled(ui::Spacing::xs);
    const float lh = ImGui::GetTextLineHeight();
    const ImVec2 box_max(max.x - pad, min.y + pad + lh * 4.0F + pad * 1.5F);
    const ImVec2 box_min(box_max.x - w - pad * 2.0F, min.y + pad);
    overlay_backdrop(box_min, box_max);
    ImDrawList* dl = ImGui::GetWindowDrawList();
    for (int i = 0; i < 4; ++i) {
        const float tw = ImGui::CalcTextSize(lines[i]).x;
        dl->AddText(ImVec2(box_max.x - pad - tw, box_min.y + pad * 0.75F + lh * static_cast<float>(i)),
                    ui::to_u32(i == 0 ? t.text : t.text_secondary), lines[i]);
    }
}

} // namespace

void draw_viewport(EditorShell& shell) {
    const EditorContext& ctx = *shell.ctx;
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(0, 0));
    const bool visible = ImGui::Begin(panel_window_name(Panel::viewport).c_str(), nullptr,
                                      ImGuiWindowFlags_NoScrollbar | ImGuiWindowFlags_NoScrollWithMouse);
    ImGui::PopStyleVar();
    if (!visible) {
        shell.viewport_hovered = false;
        ImGui::End();
        return;
    }
    const ImVec2 avail = ImGui::GetContentRegionAvail();
    shell.viewport_width = static_cast<std::uint32_t>(std::max(avail.x, 1.0F));
    shell.viewport_height = static_cast<std::uint32_t>(std::max(avail.y, 1.0F));
    ImGui::Image(static_cast<ImTextureID>(renderer::ImGuiPass::viewport_texture_id), avail);
    shell.viewport_hovered = ImGui::IsItemHovered();
    const ImVec2 min = ImGui::GetItemRectMin();
    const ImVec2 max = ImGui::GetItemRectMax();
    shell.viewport_rect[0] = min.x;
    shell.viewport_rect[1] = min.y;
    shell.viewport_rect[2] = max.x;
    shell.viewport_rect[3] = max.y;

    // Files dragged from the asset browser: glTF models are placed in front
    // of the camera; anything else is refused with a hint.
    if (ctx.instantiate_request != nullptr && ImGui::BeginDragDropTarget()) {
        if (const ImGuiPayload* payload = ImGui::AcceptDragDropPayload(
                ui::k_asset_payload, ImGuiDragDropFlags_AcceptBeforeDelivery)) {
            const std::string path(static_cast<const char*>(payload->Data));
            if (!is_gltf(path)) {
                ImGui::SetTooltip("Only glTF models (.gltf, .glb) can be placed in the scene");
            } else if (payload->IsDelivery()) {
                *ctx.instantiate_request = path;
                std::fprintf(stderr, "[editor] instantiate requested (drop): %s\n", path.c_str());
            }
        }
        ImGui::EndDragDropTarget();
    }

    if (ctx.scene != nullptr) {
        gizmo(shell, min, max);
        picking(shell, min, max);
    }
    overlay_toolbar(shell, min);
    stats_overlay(shell, min, max);
    ImGui::End();
}

} // namespace engine::editor::shell
