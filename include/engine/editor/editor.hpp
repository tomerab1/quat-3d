#pragma once

// EditorLayer — ImGui editor shell (Phase 9).
//
// Owns the ImGui context and the SDL3 platform backend (input/windowing only;
// rendering is the engine's ImGuiPass). Builds the docking shell each frame:
// the dockspace, main menu bar, the offscreen-scene Viewport panel, and the
// tool panels. Editor state (selection, panel visibility, viewport size) lives
// here, not in the ECS (CLAUDE.md). ImGui types stay out of this header —
// sources under src/editor/ are the only ImGui includes.

#include <cstdint>
#include <expected>
#include <string>

#include <entt/entity/fwd.hpp>
#include <entt/entity/entity.hpp>
#include <glm/glm.hpp>

#include "engine/core/error.hpp"

struct SDL_Window;
union SDL_Event;
struct ImDrawData;
struct ImVec2;

namespace engine::scene {
class Scene;
}

namespace engine::editor {

// Animation panel state owned by EditorLayer (editor state stays out of the
// ECS).
struct AnimationPreviewState {
    bool  show_joints  = true;
    float resume_speed = 1.0F; // speed restored by the play button after pause
};

// Physics overlay state owned by EditorLayer.
struct PhysicsDebugState {
    bool enabled      = false;
    bool show_static  = true;
    bool show_dynamic = true; // dynamic + kinematic bodies
    bool show_sensors = true;
};

// Renderer parameters the editor edits and the frame loop applies. Owned by
// the loop (initialised to the engine defaults), pointed to by EditorContext.
struct RendererSettings {
    float bloom_threshold = 1.0F;
    float bloom_knee      = 0.6F;
    float bloom_intensity = 0.06F;
    float bloom_radius    = 1.0F;
    // Set by the panel's button, consumed (and reset) by the frame loop.
    bool rebake_ibl = false;
};

// Everything the UI needs from (and feeds back to) the engine for one frame.
struct EditorContext {
    // Stats panel inputs.
    float frame_ms     = 0.0F;
    int   draw_count   = 0;
    int   entity_count = 0;
    // Scene access for the hierarchy/inspector panels (may be null).
    scene::Scene* scene = nullptr;
    // Renderer settings the panel edits (may be null).
    RendererSettings* renderer = nullptr;
    // Project root for the asset browser (empty disables the panel).
    std::string project_root;
    // Out: glTF path the user asked to instantiate (double-click or viewport
    // drop); the frame loop consumes and clears it (may be null).
    std::string* instantiate_request = nullptr;
    // The scene camera's (unjittered) view-projection — used by panels that
    // project world-space overlays into the viewport (joints, physics shapes)
    // and to build picking rays. Carries the renderer's Vulkan Y flip.
    glm::mat4 view_proj{1.0F};
    // Separate view + projection for ImGuizmo, WITHOUT the Vulkan Y flip (the
    // gizmo maps NDC to the rect itself and expects a y-up projection).
    glm::mat4 view{1.0F};
    glm::mat4 proj_no_flip{1.0F};
};

class EditorLayer {
public:
    // Creates the ImGui context (docking enabled) and the SDL3 platform
    // backend for `window`. `font_path` (TTF) replaces the default bitmap font
    // when it exists. One EditorLayer per process.
    [[nodiscard]] static std::expected<EditorLayer, core::Error>
    create(SDL_Window* window, const std::string& font_path = {});

    EditorLayer() = default;
    ~EditorLayer();
    EditorLayer(EditorLayer&& other) noexcept;
    EditorLayer& operator=(EditorLayer&& other) noexcept;
    EditorLayer(const EditorLayer&) = delete;
    EditorLayer& operator=(const EditorLayer&) = delete;

    // False for a default-constructed (or failed/disabled) layer — every other
    // method is then a no-op, so callers can use the layer unconditionally.
    [[nodiscard]] bool active() const { return initialized_; }

    // Forward an SDL event to ImGui. Call for every event, before the engine's
    // own handling; use wants_mouse()/wants_keyboard() to suppress game input.
    void process_event(const SDL_Event& event);
    [[nodiscard]] bool wants_mouse() const;
    [[nodiscard]] bool wants_keyboard() const;

    // The Viewport panel's content size (pixels) and hover state from the last
    // built frame — the scene renders at this size, and camera input is only
    // captured while the cursor is over the panel. Zero size until the first
    // frame; callers fall back to the swapchain extent.
    [[nodiscard]] std::uint32_t viewport_width() const { return viewport_width_; }
    [[nodiscard]] std::uint32_t viewport_height() const { return viewport_height_; }
    [[nodiscard]] bool viewport_hovered() const { return viewport_hovered_; }

    // The entity selected in the hierarchy (entt::null when none).
    [[nodiscard]] entt::entity selected() const { return selected_; }

    // Frame flow: begin_frame() right after event processing, build_ui() once
    // the frame's numbers are known, end_frame() to obtain the draw data for
    // ImGuiPass::add_to_graph.
    void begin_frame();
    void build_ui(const EditorContext& ctx);
    [[nodiscard]] const ImDrawData* end_frame();

private:
    void destroy() noexcept;
    void build_dock_layout(unsigned int dockspace_id);
    void build_viewport_panel(const EditorContext& ctx);
    void build_stats_panel(const EditorContext& ctx);
    void build_gizmo(const EditorContext& ctx, const ImVec2& rect_min, const ImVec2& rect_max);
    void handle_picking(const EditorContext& ctx, const ImVec2& rect_min,
                        const ImVec2& rect_max);

    bool initialized_  = false;
    bool layout_built_ = false;
    bool show_stats_   = true;
    bool show_demo_    = false;

    std::uint32_t viewport_width_   = 0;
    std::uint32_t viewport_height_  = 0;
    bool          viewport_hovered_ = false;
    // Viewport image rect in screen pixels (min x/y, max x/y) — overlays
    // (joints, gizmos) project into this.
    float viewport_rect_[4]{0.0F, 0.0F, 0.0F, 0.0F};

    AnimationPreviewState anim_state_;
    PhysicsDebugState     physics_state_;

    int gizmo_op_ = 0; // 0 translate, 1 rotate, 2 scale (W/E/R over the viewport)

    entt::entity selected_ = entt::null;
};

} // namespace engine::editor
