#include "engine/editor/editor.hpp"

#include <algorithm>
#include <string>
#include <utility>

#include <imgui.h>
#include <imgui_internal.h> // DockBuilder API (initial layout)

#include "animation_preview.hpp"
#include "asset_browser.hpp"
#include "engine/renderer/imgui_pass.hpp" // viewport_texture_id
#include "inspector.hpp"
#include "scene_hierarchy.hpp"
#include "vendor/imgui_impl_sdl3.h"

namespace engine::editor {

namespace {

[[nodiscard]] std::unexpected<core::Error> fail(std::string what) {
    return std::unexpected(core::Error{std::move(what)});
}

} // namespace

std::expected<EditorLayer, core::Error> EditorLayer::create(SDL_Window* window) {
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
    ImGui::StyleColorsDark();

    if (!ImGui_ImplSDL3_InitForOther(window)) {
        ImGui::DestroyContext();
        return fail("editor: ImGui_ImplSDL3_Init failed");
    }

    EditorLayer out;
    out.initialized_ = true;
    return out;
}

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
    }
    return *this;
}

void EditorLayer::destroy() noexcept {
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

void EditorLayer::begin_frame() {
    if (!initialized_) return;
    ImGui_ImplSDL3_NewFrame();
    ImGui::NewFrame();
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
    ImGui::DockBuilderDockWindow("Inspector", right);
    ImGui::DockBuilderDockWindow("Renderer", right);
    ImGui::DockBuilderDockWindow("Assets", bottom);
    ImGui::DockBuilderDockWindow("Animation", bottom);
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
            }
            ImGui::EndDragDropTarget();
        }
    } else {
        viewport_hovered_ = false;
    }
    ImGui::End();
    ImGui::PopStyleVar();
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
    }
    ImGui::End();
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
        if (ImGui::BeginMenu("View")) {
            ImGui::MenuItem("Stats", nullptr, &show_stats_);
            ImGui::MenuItem("ImGui Demo", nullptr, &show_demo_);
            ImGui::EndMenu();
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
    }
    if (ctx.renderer != nullptr) {
        draw_renderer_panel(*ctx.renderer);
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
