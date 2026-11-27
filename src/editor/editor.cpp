#include "engine/editor/editor.hpp"

#include <algorithm>
#include <cstdio>
#include <filesystem>
#include <limits>
#include <string>
#include <utility>

#include <imgui.h>
#include <imgui_internal.h> // DockBuilder API (initial layout)

#include <ImGuizmo.h> // must follow imgui.h

#include "engine/core/utf8_path.hpp"
#include "engine/scene/components.hpp"
#include "engine/scene/scene.hpp"

#include "animation_preview.hpp"
#include "asset_browser.hpp"
#include "behavior_panel.hpp"
#include "physics_debug.hpp"
#include "picking.hpp"
#include "engine/renderer/imgui_pass.hpp" // viewport_texture_id
#include "inspector.hpp"
#include "scene_hierarchy.hpp"
#include "vendor/imgui_impl_sdl3.h"

namespace engine::editor {

namespace {

[[nodiscard]] std::unexpected<core::Error> fail(std::string what) {
    return std::unexpected(core::Error{std::move(what)});
}

// Polished dark theme: flatter panels, subtle accents, consistent rounding.
void apply_theme() {
    ImGuiStyle& style = ImGui::GetStyle();
    style.WindowRounding = 4.0F;
    style.ChildRounding = 4.0F;
    style.FrameRounding = 3.0F;
    style.GrabRounding = 3.0F;
    style.TabRounding = 4.0F;
    style.PopupRounding = 4.0F;
    style.WindowBorderSize = 0.0F;
    style.FrameBorderSize = 0.0F;
    style.WindowPadding = ImVec2(8, 8);
    style.FramePadding = ImVec2(6, 4);
    style.ItemSpacing = ImVec2(7, 5);
    style.IndentSpacing = 16.0F;

    ImVec4* c = style.Colors;
    const ImVec4 bg(0.106F, 0.113F, 0.125F, 1.0F);
    const ImVec4 bg_dark(0.082F, 0.086F, 0.094F, 1.0F);
    const ImVec4 panel(0.149F, 0.157F, 0.173F, 1.0F);
    const ImVec4 panel_hover(0.21F, 0.22F, 0.24F, 1.0F);
    const ImVec4 accent(0.20F, 0.47F, 0.84F, 1.0F);
    const ImVec4 accent_hover(0.26F, 0.55F, 0.93F, 1.0F);
    const ImVec4 text(0.88F, 0.89F, 0.90F, 1.0F);
    c[ImGuiCol_Text] = text;
    c[ImGuiCol_TextDisabled] = ImVec4(0.48F, 0.50F, 0.53F, 1.0F);
    c[ImGuiCol_WindowBg] = bg;
    c[ImGuiCol_ChildBg] = bg;
    c[ImGuiCol_PopupBg] = bg_dark;
    c[ImGuiCol_Border] = ImVec4(0.0F, 0.0F, 0.0F, 0.35F);
    c[ImGuiCol_FrameBg] = panel;
    c[ImGuiCol_FrameBgHovered] = panel_hover;
    c[ImGuiCol_FrameBgActive] = panel_hover;
    c[ImGuiCol_TitleBg] = bg_dark;
    c[ImGuiCol_TitleBgActive] = bg_dark;
    c[ImGuiCol_MenuBarBg] = bg_dark;
    c[ImGuiCol_ScrollbarBg] = bg_dark;
    c[ImGuiCol_ScrollbarGrab] = panel;
    c[ImGuiCol_ScrollbarGrabHovered] = panel_hover;
    c[ImGuiCol_CheckMark] = accent;
    c[ImGuiCol_SliderGrab] = accent;
    c[ImGuiCol_SliderGrabActive] = accent_hover;
    c[ImGuiCol_Button] = panel;
    c[ImGuiCol_ButtonHovered] = panel_hover;
    c[ImGuiCol_ButtonActive] = accent;
    c[ImGuiCol_Header] = panel;
    c[ImGuiCol_HeaderHovered] = panel_hover;
    c[ImGuiCol_HeaderActive] = accent;
    c[ImGuiCol_Separator] = ImVec4(0.0F, 0.0F, 0.0F, 0.4F);
    c[ImGuiCol_ResizeGrip] = panel;
    c[ImGuiCol_ResizeGripHovered] = accent;
    c[ImGuiCol_Tab] = bg_dark;
    c[ImGuiCol_TabHovered] = accent_hover;
    c[ImGuiCol_TabSelected] = panel;
    c[ImGuiCol_TabDimmed] = bg_dark;
    c[ImGuiCol_TabDimmedSelected] = panel;
    c[ImGuiCol_DockingPreview] = ImVec4(accent.x, accent.y, accent.z, 0.5F);
    c[ImGuiCol_DockingEmptyBg] = bg_dark;
    c[ImGuiCol_DragDropTarget] = accent_hover;
    c[ImGuiCol_NavCursor] = accent;
}

} // namespace

std::expected<EditorLayer, core::Error> EditorLayer::create(SDL_Window* window,
                                                            const std::string& font_path) {
    if (window == nullptr) {
        return fail("editor: null window");
    }
    if (ImGui::GetCurrentContext() != nullptr) {
        return fail("editor: an ImGui context already exists");
    }

    IMGUI_CHECKVERSION();
    ImGui::CreateContext();
    ImGuiIO& io = ImGui::GetIO();
    io.ConfigFlags |= ImGuiConfigFlags_DockingEnable;
    io.ConfigFlags |= ImGuiConfigFlags_NavEnableKeyboard;
    io.IniFilename = nullptr; // panel layout persistence arrives with the real layout
    io.BackendRendererName = "quat3d (descriptor buffer)";

    std::error_code ec;
    if (!font_path.empty() && std::filesystem::is_regular_file(font_path, ec)) {
        io.Fonts->AddFontFromFileTTF(font_path.c_str(), 16.0F);
    }
    ImGui::StyleColorsDark();
    apply_theme();

    if (!ImGui_ImplSDL3_InitForOther(window)) {
        ImGui::DestroyContext();
        return fail("editor: ImGui_ImplSDL3_Init failed");
    }

    EditorLayer out;
    out.initialized_ = true;
    return out;
}

EditorLayer::EditorLayer() = default;

EditorLayer::~EditorLayer() {
    destroy();
}

EditorLayer::EditorLayer(EditorLayer&& other) noexcept {
    *this = std::move(other);
}

EditorLayer& EditorLayer::operator=(EditorLayer&& other) noexcept {
    if (this != &other) {
        destroy();
        initialized_ = std::exchange(other.initialized_, false);
        layout_built_ = other.layout_built_;
        show_stats_ = other.show_stats_;
        show_demo_ = other.show_demo_;
        viewport_width_ = other.viewport_width_;
        viewport_height_ = other.viewport_height_;
        viewport_hovered_ = other.viewport_hovered_;
        selected_ = other.selected_;
        behavior_panel_ = std::move(other.behavior_panel_);
    }
    return *this;
}

void EditorLayer::destroy() noexcept {
    behavior_panel_.reset(); // its node-editor context needs ImGui alive
    if (initialized_) {
        ImGui_ImplSDL3_Shutdown();
        ImGui::DestroyContext();
        initialized_ = false;
    }
}

void EditorLayer::process_event(const SDL_Event& event) {
    if (initialized_) ImGui_ImplSDL3_ProcessEvent(&event);
}

bool EditorLayer::wants_mouse() const {
    return initialized_ && ImGui::GetIO().WantCaptureMouse;
}

bool EditorLayer::wants_keyboard() const {
    return initialized_ && ImGui::GetIO().WantCaptureKeyboard;
}

bool EditorLayer::wants_text_input() const {
    return initialized_ && ImGui::GetIO().WantTextInput;
}

void EditorLayer::begin_frame() {
    if (!initialized_) return;
    ImGui_ImplSDL3_NewFrame();
    ImGui::NewFrame();
    ImGuizmo::BeginFrame();
}

// One-time split of the fullscreen dockspace: Viewport fills the centre,
// Hierarchy + Stats dock left, Inspector right, Assets/Animation bottom.
// Windows from later slices are docked by name up front — DockBuilder accepts
// names for windows that do not exist yet.
void EditorLayer::build_dock_layout(unsigned int dockspace_id) {
    ImGui::DockBuilderRemoveNode(dockspace_id);
    ImGui::DockBuilderAddNode(dockspace_id, ImGuiDockNodeFlags_DockSpace);
    ImGui::DockBuilderSetNodeSize(dockspace_id, ImGui::GetMainViewport()->WorkSize);

    ImGuiID centre = dockspace_id;
    const ImGuiID right = ImGui::DockBuilderSplitNode(centre, ImGuiDir_Right, 0.22F, nullptr,
                                                      &centre);
    const ImGuiID left = ImGui::DockBuilderSplitNode(centre, ImGuiDir_Left, 0.24F, nullptr,
                                                     &centre);
    const ImGuiID bottom = ImGui::DockBuilderSplitNode(centre, ImGuiDir_Down, 0.28F, nullptr,
                                                       &centre);
    ImGuiID left_bottom = 0;
    const ImGuiID left_top = ImGui::DockBuilderSplitNode(left, ImGuiDir_Up, 0.65F, nullptr,
                                                         &left_bottom);

    ImGui::DockBuilderDockWindow("Viewport", centre);
    ImGui::DockBuilderDockWindow("Hierarchy", left_top);
    ImGui::DockBuilderDockWindow("Stats", left_bottom);
    ImGui::DockBuilderDockWindow("Physics", left_bottom);
    ImGui::DockBuilderDockWindow("Inspector", right);
    ImGui::DockBuilderDockWindow("Renderer", right);
    ImGui::DockBuilderDockWindow("Assets", bottom);
    ImGui::DockBuilderDockWindow("Animation", bottom);
    ImGui::DockBuilderDockWindow(behavior_panel_title, bottom);
    ImGui::DockBuilderFinish(dockspace_id);
}

void EditorLayer::build_viewport_panel(const EditorContext& ctx) {
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(0, 0));
    if (ImGui::Begin("Viewport")) {
        const ImVec2 avail = ImGui::GetContentRegionAvail();
        viewport_width_ = static_cast<std::uint32_t>(std::max(avail.x, 1.0F));
        viewport_height_ = static_cast<std::uint32_t>(std::max(avail.y, 1.0F));
        ImGui::Image(static_cast<ImTextureID>(renderer::ImGuiPass::viewport_texture_id), avail);
        viewport_hovered_ = ImGui::IsItemHovered();
        const ImVec2 rect_min = ImGui::GetItemRectMin();
        const ImVec2 rect_max = ImGui::GetItemRectMax();
        viewport_rect_[0] = rect_min.x;
        viewport_rect_[1] = rect_min.y;
        viewport_rect_[2] = rect_max.x;
        viewport_rect_[3] = rect_max.y;

        // Drop target for the asset browser's glTF drag payload.
        if (ctx.instantiate_request != nullptr && ImGui::BeginDragDropTarget()) {
            if (const ImGuiPayload* payload =
                    ImGui::AcceptDragDropPayload(asset_drag_payload)) {
                *ctx.instantiate_request = static_cast<const char*>(payload->Data);
                std::fprintf(stderr, "[editor] instantiate requested (drop): %s\n",
                             ctx.instantiate_request->c_str());
            }
            ImGui::EndDragDropTarget();
        }

        // W/E/R switch the gizmo while hovering the viewport — except during a
        // right-drag, when WASD belongs to the fly camera.
        const ImGuiIO& io = ImGui::GetIO();
        if (viewport_hovered_ && !io.WantTextInput && !io.MouseDown[ImGuiMouseButton_Right]) {
            if (ImGui::IsKeyPressed(ImGuiKey_W)) gizmo_op_ = 0;
            if (ImGui::IsKeyPressed(ImGuiKey_E)) gizmo_op_ = 1;
            if (ImGui::IsKeyPressed(ImGuiKey_R)) gizmo_op_ = 2;
        }

        if (ctx.scene != nullptr) {
            build_gizmo(ctx, rect_min, rect_max);
            handle_picking(ctx, rect_min, rect_max);
        }

        // Mode toolbar floating in the panel's top-left corner.
        ImGui::SetCursorScreenPos(ImVec2(rect_min.x + 8.0F, rect_min.y + 8.0F));
        const char* labels[3] = {"move (W)", "rotate (E)", "scale (R)"};
        for (int i = 0; i < 3; ++i) {
            if (i > 0) ImGui::SameLine();
            const bool active_op = gizmo_op_ == i;
            if (active_op) {
                ImGui::PushStyleColor(ImGuiCol_Button, ImGui::GetStyleColorVec4(
                                                           ImGuiCol_ButtonActive));
            }
            if (ImGui::SmallButton(labels[i])) gizmo_op_ = i;
            if (active_op) ImGui::PopStyleColor();
        }
    } else {
        viewport_hovered_ = false;
    }
    ImGui::End();
    ImGui::PopStyleVar();
}

// ImGuizmo manipulator for the selected entity's Transform. Operates on the
// WORLD matrix, then converts back to local through the parent's world.
void EditorLayer::build_gizmo(const EditorContext& ctx, const ImVec2& rect_min,
                              const ImVec2& rect_max) {
    entt::registry& r = ctx.scene->registry();
    if (selected_ == entt::null || !r.valid(selected_)) return;
    auto* transform = r.try_get<scene::Transform>(selected_);
    if (transform == nullptr) return;

    ImGuizmo::SetOrthographic(false);
    ImGuizmo::SetDrawlist(ImGui::GetWindowDrawList());
    ImGuizmo::SetRect(rect_min.x, rect_min.y, rect_max.x - rect_min.x,
                      rect_max.y - rect_min.y);

    const ImGuizmo::OPERATION op = gizmo_op_ == 0   ? ImGuizmo::TRANSLATE
                                   : gizmo_op_ == 1 ? ImGuizmo::ROTATE
                                                    : ImGuizmo::SCALE;
    glm::mat4 world = transform->world;
    if (ImGuizmo::Manipulate(&ctx.view[0][0], &ctx.proj_no_flip[0][0], op, ImGuizmo::WORLD,
                             &world[0][0])) {
        glm::mat4 parent_world(1.0F);
        if (const auto* parent = r.try_get<scene::Parent>(selected_);
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
// world-space-transformed local AABB (pick_entity); the nearest hit becomes
// the selection.
void EditorLayer::handle_picking(const EditorContext& ctx, const ImVec2& rect_min,
                                 const ImVec2& rect_max) {
    if (!ImGui::IsItemClicked(ImGuiMouseButton_Left)) return;
    if (ImGuizmo::IsOver() || ImGuizmo::IsUsing()) return;

    const ImVec2 mouse = ImGui::GetMousePos();
    const float w = std::max(rect_max.x - rect_min.x, 1.0F);
    const float h = std::max(rect_max.y - rect_min.y, 1.0F);
    // The flipped view-proj maps NDC y downward, matching screen coordinates.
    const glm::vec2 ndc((mouse.x - rect_min.x) / w * 2.0F - 1.0F,
                        (mouse.y - rect_min.y) / h * 2.0F - 1.0F);
    const glm::mat4 inv_vp = glm::inverse(ctx.view_proj);
    const glm::vec4 near_h = inv_vp * glm::vec4(ndc, 0.0F, 1.0F);
    const glm::vec4 far_h = inv_vp * glm::vec4(ndc, 1.0F, 1.0F);
    const glm::vec3 origin = glm::vec3(near_h) / near_h.w;
    const glm::vec3 target = glm::vec3(far_h) / far_h.w;
    const glm::vec3 dir = glm::normalize(target - origin);

    // Ctrl/Shift+click: command rays for the frame loop (walk-here / add
    // waypoint) — selection stays untouched.
    const ImGuiIO& io = ImGui::GetIO();
    if ((io.KeyCtrl || io.KeyShift) && ctx.ground_click != nullptr) {
        EditorContext::GroundClick click;
        click.origin = origin;
        click.dir = dir;
        click.add_waypoint = io.KeyShift;
        *ctx.ground_click = click;
        return;
    }

    const entt::entity hit = pick_entity(ctx.scene->registry(), origin, dir);
    if (hit != entt::null) selected_ = hit;
}

void EditorLayer::build_stats_panel(const EditorContext& ctx) {
    if (!show_stats_) return;
    if (ImGui::Begin("Stats", &show_stats_)) {
        const ImGuiIO& io = ImGui::GetIO();
        ImGui::Text("%.1f fps (%.2f ms)", io.Framerate,
                    io.Framerate > 0.0F ? 1000.0F / io.Framerate : 0.0F);
        ImGui::Text("frame: %.2f ms", ctx.frame_ms);
        ImGui::Text("draws: %d", ctx.draw_count);
        ImGui::Text("entities: %d", ctx.entity_count);
        ImGui::Text("viewport: %ux%u", viewport_width_, viewport_height_);
        build_shader_status(ctx);
    }
    ImGui::End();
}

// Shader hot reload (R2): one row per Slang source; failed shaders open by
// default with the compiler diagnostics (the previous pipeline stays live).
void EditorLayer::build_shader_status(const EditorContext& ctx) {
    if (ctx.shader_statuses == nullptr) return;
    const std::vector<rhi::ShaderStatus>& shaders = *ctx.shader_statuses;
    std::size_t errors = 0;
    std::size_t compiling = 0;
    std::size_t restart = 0;
    for (const rhi::ShaderStatus& s : shaders) {
        if (s.state == rhi::ShaderStatus::State::error) ++errors;
        if (s.state == rhi::ShaderStatus::State::compiling) ++compiling;
        if (s.state == rhi::ShaderStatus::State::needs_restart) ++restart;
    }
    char header[128];
    std::snprintf(header, sizeof(header), "Shaders (%zu, %zu error%s%s%s)###shaders",
                  shaders.size(), errors, errors == 1 ? "" : "s", compiling > 0 ? ", compiling" : "",
                  restart > 0 ? ", restart to apply" : "");
    if (errors > 0) ImGui::SetNextItemOpen(true, ImGuiCond_Always);
    if (!ImGui::CollapsingHeader(header)) return;

    const ImVec4 colour_error(1.0F, 0.4F, 0.35F, 1.0F);
    const ImVec4 colour_ok(0.45F, 0.85F, 0.45F, 1.0F);
    const ImVec4 colour_busy(0.95F, 0.8F, 0.3F, 1.0F);
    const ImVec4 colour_restart(0.45F, 0.7F, 1.0F, 1.0F);
    for (const rhi::ShaderStatus& s : shaders) {
        ImGui::PushID(s.name.c_str());
        const std::string_view state = rhi::to_string(s.state);
        switch (s.state) {
        case rhi::ShaderStatus::State::error:
            ImGui::TextColored(colour_error, "%-9.*s", static_cast<int>(state.size()), state.data());
            break;
        case rhi::ShaderStatus::State::ok:
            ImGui::TextColored(colour_ok, "%-9.*s", static_cast<int>(state.size()), state.data());
            break;
        case rhi::ShaderStatus::State::compiling:
            ImGui::TextColored(colour_busy, "%-9.*s", static_cast<int>(state.size()), state.data());
            break;
        case rhi::ShaderStatus::State::needs_restart:
            ImGui::TextColored(colour_restart, "%-9.*s", static_cast<int>(state.size()), state.data());
            break;
        case rhi::ShaderStatus::State::cooked:
            ImGui::TextDisabled("%-9.*s", static_cast<int>(state.size()), state.data());
            break;
        }
        ImGui::SameLine();
        if (s.reloads > 0) {
            ImGui::Text("%s  (x%u, %u pipeline%s)", s.name.c_str(), s.reloads, s.pipelines,
                        s.pipelines == 1 ? "" : "s");
        } else {
            ImGui::TextUnformatted(s.name.c_str());
        }
        if (!s.message.empty()) {
            const bool failed = s.state == rhi::ShaderStatus::State::error;
            if (failed) ImGui::SetNextItemOpen(true, ImGuiCond_Once);
            const bool restart_note = s.state == rhi::ShaderStatus::State::needs_restart;
            if (restart_note) ImGui::SetNextItemOpen(true, ImGuiCond_Once);
            if (ImGui::TreeNode(failed ? "diagnostics" : restart_note ? "not applied" : "warnings")) {
                ImGui::PushTextWrapPos(0.0F);
                if (failed) ImGui::PushStyleColor(ImGuiCol_Text, colour_error);
                ImGui::TextUnformatted(s.message.c_str());
                if (failed) ImGui::PopStyleColor();
                ImGui::PopTextWrapPos();
                if (ImGui::SmallButton("Copy")) ImGui::SetClipboardText(s.message.c_str());
                ImGui::TreePop();
            }
        }
        ImGui::PopID();
    }
}

void EditorLayer::build_ui(const EditorContext& ctx) {
    if (!initialized_) return;

    const ImGuiID dockspace_id =
        ImGui::DockSpaceOverViewport(0, ImGui::GetMainViewport(), ImGuiDockNodeFlags_None);
    if (!layout_built_) {
        layout_built_ = true;
        build_dock_layout(dockspace_id);
    }

    if (ImGui::BeginMainMenuBar()) {
        if (ImGui::BeginMenu("File")) {
            ImGui::SetNextItemWidth(260.0F);
            ImGui::InputText("scene file", scene_file_, sizeof(scene_file_));
            const auto resolve = [&]() {
                // ImGui text and the requests are UTF-8.
                std::filesystem::path p = core::path_from_utf8(scene_file_);
                if (p.is_relative() && !ctx.project_root.empty()) {
                    p = core::path_from_utf8(ctx.project_root) / p;
                }
                return core::path_to_utf8(p);
            };
            if (ctx.save_request != nullptr && ImGui::MenuItem("Save scene")) {
                *ctx.save_request = resolve();
            }
            if (ctx.load_request != nullptr && ImGui::MenuItem("Load scene")) {
                *ctx.load_request = resolve();
            }
            ImGui::EndMenu();
        }
        if (ImGui::BeginMenu("View")) {
            ImGui::MenuItem("Stats", nullptr, &show_stats_);
            ImGui::MenuItem("ImGui Demo", nullptr, &show_demo_);
            ImGui::EndMenu();
        }
        // Play/Stop, centred-ish in the bar. The loop owns the transition
        // (scene snapshot + physics world); this just flips the flag.
        if (ctx.play_mode != nullptr) {
            ImGui::SetCursorPosX(ImGui::GetWindowWidth() * 0.5F - 30.0F);
            const bool playing = *ctx.play_mode;
            if (playing) {
                ImGui::PushStyleColor(ImGuiCol_Button, ImVec4(0.70F, 0.25F, 0.22F, 1.0F));
                ImGui::PushStyleColor(ImGuiCol_ButtonHovered, ImVec4(0.80F, 0.32F, 0.28F, 1.0F));
            }
            if (ImGui::SmallButton(playing ? "[] Stop" : "|> Play")) {
                *ctx.play_mode = !playing;
            }
            if (playing) {
                ImGui::PopStyleColor(2);
                if (ctx.play_use_scene_camera != nullptr) {
                    ImGui::Checkbox("Game camera", ctx.play_use_scene_camera);
                    if (ImGui::IsItemHovered()) {
                        ImGui::SetTooltip("View through the scene's active Camera entity\n"
                                          "(unchecked: keep flying the editor camera)");
                    }
                }
                ImGui::TextDisabled("simulating");
            }
        }
        ImGui::EndMainMenuBar();
    }

    build_viewport_panel(ctx);
    build_stats_panel(ctx);
    if (ctx.scene != nullptr) {
        draw_scene_hierarchy(*ctx.scene, selected_);
        draw_inspector(*ctx.scene, selected_);
        draw_animation_preview(*ctx.scene, selected_, ctx.view_proj, viewport_rect_,
                               anim_state_);
        draw_physics_debug(*ctx.scene, ctx.view_proj, viewport_rect_, physics_state_,
                           ctx.build_navmesh_request, ctx.nav_edges, ctx.nav_status);
        if (behavior_panel_ == nullptr) behavior_panel_ = std::make_unique<BehaviorPanel>();
        behavior_panel_->draw(*ctx.scene, selected_);
    }
    if (ctx.renderer != nullptr) {
        draw_renderer_panel(*ctx.renderer, ctx.scene);
    }
    if (!ctx.project_root.empty() && ctx.instantiate_request != nullptr) {
        draw_asset_browser(ctx.project_root, *ctx.instantiate_request);
    }
    if (show_demo_) {
        ImGui::ShowDemoWindow(&show_demo_);
    }
}

const ImDrawData* EditorLayer::end_frame() {
    if (!initialized_) return nullptr;
    ImGui::Render();
    return ImGui::GetDrawData();
}

} // namespace engine::editor
