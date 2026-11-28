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
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include <entt/entity/fwd.hpp>
#include <entt/entity/entity.hpp>
#include <glm/glm.hpp>

#include "engine/core/error.hpp"
#include "engine/rhi/shader_status.hpp"

struct SDL_Window;
union SDL_Event;
struct ImDrawData;
struct ImVec2;

namespace engine::core {
class SimClock;
}
namespace engine::renderer {
struct RenderSettings;
}
namespace engine::scene {
class Scene;
}
namespace engine::script {
class ScriptRuntime;
}

namespace engine::editor {

class BehaviorPanel;
namespace shell {
class EditorShell;
}

// Animation panel state owned by EditorLayer (editor state stays out of the
// ECS).
struct AnimationPreviewState {
    bool  show_joints  = true;
    float resume_speed = 1.0F; // speed restored by the play button after pause
};

// Physics overlay state owned by EditorLayer. The overlay defaults ON so a
// freshly added collider is immediately visible.
struct PhysicsDebugState {
    bool enabled      = true;
    bool show_static  = true;
    bool show_dynamic = true; // dynamic + kinematic bodies
    bool show_sensors = true;
    bool show_navmesh = true; // overlay the baked navmesh polygons (13.1)
};

// Toast severity for EditorLayer::notify.
enum class NoticeKind : std::uint8_t { info, success, warning, error };

// The remote-control endpoint as the status bar shows it.
struct RemoteStatus {
    bool          enabled = false; // QUAT_REMOTE set and listening
    std::uint16_t port = 0;
    std::size_t   clients = 0; // connected tools (the MCP bridge)
};

// Everything the UI needs from (and feeds back to) the engine for one frame.
struct EditorContext {
    // Stats panel inputs.
    float frame_ms     = 0.0F;
    int   draw_count   = 0;
    int   entity_count = 0;
    // Shader hot-reload state (R2), listed in the Stats panel; null when hot
    // reload is off.
    const std::vector<rhi::ShaderStatus>* shader_statuses = nullptr;
    // Gameplay scripts (G7): the Stats panel lists their status and the
    // inspector describes Script components (may be null).
    script::ScriptRuntime* scripts = nullptr;
    // Scene access for the hierarchy/inspector panels (may be null).
    scene::Scene* scene = nullptr;
    // Renderer settings the Renderer panel edits (owned by the frame
    // renderer; may be null).
    renderer::RenderSettings* renderer = nullptr;
    // R21: the environment's HDRI — its path and a one-line status for the
    // Renderer panel, and the path its Load / Clear buttons request
    // (consumed by the frame loop; "" clears the HDRI; may be null).
    std::string                 hdri_path;
    std::string                 hdri_status;
    std::optional<std::string>* hdri_request = nullptr;
    // Project root for the asset browser (empty disables the panel).
    std::string project_root;
    // Out: glTF path the user asked to instantiate (double-click or viewport
    // drop); the frame loop consumes and clears it (may be null).
    std::string* instantiate_request = nullptr;
    // Out: scene file paths from the File menu; consumed by the frame loop.
    std::string* save_request = nullptr;
    std::string* load_request = nullptr;
    // In/out play-mode flag (the menu-bar Play/Stop button toggles it; the
    // frame loop snapshots/restores the scene on the transition). Null hides
    // the button (e.g. while a physics demo owns the world).
    bool* play_mode = nullptr;
    // In/out: while playing, render the viewport through the scene's active
    // Camera entity instead of the editor fly camera (menu-bar checkbox,
    // shown only in play mode). The fly camera remains the default so Play
    // never takes movement away from the user.
    bool* play_use_scene_camera = nullptr;
    // Out: the Physics panel's "Build navmesh" button; the frame loop bakes a
    // NavMesh from the current static geometry and clears the flag (may be null).
    bool* build_navmesh_request = nullptr;
    // Out: Ctrl/Shift+click in the viewport — a picking ray the frame loop
    // resolves against the ground. Ctrl commands the selected agent to walk
    // there; Shift appends a patrol waypoint (may be null).
    struct GroundClick {
        glm::vec3 origin{0.0F};
        glm::vec3 dir{0.0F};
        bool      add_waypoint = false; // true = Shift (append), false = Ctrl (go)
    };
    std::optional<GroundClick>* ground_click = nullptr;
    // In: polygon edges of the baked navmesh for the overlay (may be null).
    const std::vector<std::pair<glm::vec3, glm::vec3>>* nav_edges = nullptr;
    // In: the last navmesh bake / invalidation, shown in the Physics panel
    // (failed bakes must not go unnoticed; may be null).
    const std::string* nav_status = nullptr;
    // The scene camera's (unjittered) view-projection — used by panels that
    // project world-space overlays into the viewport (joints, physics shapes)
    // and to build picking rays. Carries the renderer's Vulkan Y flip.
    glm::mat4 view_proj{1.0F};
    // Separate view + projection for ImGuizmo, WITHOUT the Vulkan Y flip (the
    // gizmo maps NDC to the rect itself and expects a y-up projection).
    glm::mat4 view{1.0F};
    glm::mat4 proj_no_flip{1.0F};

    // ---- Shell (ROADMAP E8.2) --------------------------------------------
    // The simulation clock (A5) for Pause / Step; null disables them.
    core::SimClock* sim_clock = nullptr;
    // Remote-control endpoint state for the status bar.
    RemoteStatus remote;
    // In/out: the fly camera's speed multiplier (toolbar); may be null.
    float* camera_speed = nullptr;
    // Out: File > New; the frame loop replaces the scene with an empty one.
    bool* new_scene_request = nullptr;
    // Real frame time (seconds) for toasts and the status bar.
    float dt = 0.0F;
};

class EditorLayer {
public:
    // Creates the ImGui context (docking enabled) and the SDL3 platform
    // backend for `window`, with the editor fonts loaded from
    // `<project_root>/assets/fonts` (ImGui's built-in font when missing).
    // One EditorLayer per process.
    [[nodiscard]] static std::expected<EditorLayer, core::Error>
    create(SDL_Window* window, const std::string& project_root);

    EditorLayer(); // out of line: BehaviorPanel is incomplete here
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
    // True only while a text field is being edited — gates game movement keys
    // (wants_keyboard also fires for UI navigation, which is too aggressive).
    [[nodiscard]] bool wants_text_input() const;

    // "dark" (default) or "light"; applied from the next frame. Wins over the
    // theme saved in .quat/editor.ini (QUAT_EDITOR_THEME sets it at startup).
    void set_theme(std::string_view name);
    // True once after begin_frame() rebuilt the font atlas for a new display
    // scale: the renderer must re-upload it before drawing this frame's UI.
    [[nodiscard]] bool take_font_atlas_changed();

    // The Viewport panel's content size (pixels) and hover state from the last
    // built frame — the scene renders at this size, and camera input is only
    // captured while the cursor is over the panel. Zero size until the first
    // frame; callers fall back to the swapchain extent.
    [[nodiscard]] std::uint32_t viewport_width() const;
    [[nodiscard]] std::uint32_t viewport_height() const;
    [[nodiscard]] bool viewport_hovered() const;

    // The entity selected in the hierarchy (entt::null when none).
    [[nodiscard]] entt::entity selected() const;
    void set_selected(entt::entity entity);
    // Brings the panel titled `title` to the front of its dock tab bar over
    // the next few frames (startup state for headless captures).
    void focus_panel(std::string title);
    // A toast in the editor's bottom-right corner (scene saved, load failed).
    void notify(NoticeKind kind, std::string text);
    // False: start from the default layout and never write the layout file
    // (QUAT_EDITOR_LAYOUT=default; headless captures, tests).
    void use_saved_layout(bool use);

    // Frame flow: begin_frame() right after event processing, build_ui() once
    // the frame's numbers are known, end_frame() to obtain the draw data for
    // ImGuiPass::add_to_graph.
    void begin_frame();
    void build_ui(const EditorContext& ctx);
    [[nodiscard]] const ImDrawData* end_frame();

private:
    void destroy() noexcept;

    bool initialized_ = false;
    bool font_atlas_changed_ = false;
    // Everything the UI keeps between frames (src/editor/shell/editor_shell.hpp).
    std::unique_ptr<shell::EditorShell> shell_;
};

} // namespace engine::editor
