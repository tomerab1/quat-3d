#pragma once

// EditorBridge — the editor layer's side of the `game` frame loop (app layer,
// editor builds only). Owns the EditorLayer (ImGui shell) and the requests the
// UI raises, and applies them at fixed points of the frame:
//
//   begin_frame()          after remote requests: starts the UI frame, then
//                          applies Play/Stop (scene snapshot + the world's
//                          play physics), scene save/load and glTF instantiate
//   handle_ground_click()  after terrain upkeep: Ctrl/Shift+click in the
//                          viewport commands the selected nav agent
//   build_ui()             inside the render graph build: panels + draw data
//
// Not movable: the UI, the renderer and the remote bridge hold pointers into it.

#ifdef ENGINE_EDITOR

#include <functional>
#include <optional>
#include <string>

#include <glm/glm.hpp>
#include <vulkan/vulkan.h>

#include "app/fly_camera.hpp"
#include "app/play_mode.hpp"
#include "app/world_runtime.hpp"
#include "engine/editor/editor.hpp"

struct ImDrawData;
struct SDL_Window;
union SDL_Event;

namespace engine::asset {
class AssetManager;
}
namespace engine::renderer {
struct RenderSettings;
}
namespace engine::rhi {
class Device;
class GpuAllocator;
class ShaderHotReload;
} // namespace engine::rhi
namespace engine::scene {
class Scene;
struct CameraMatrices;
} // namespace engine::scene

namespace quat::app {

class RemoteBridge;
struct RemoteBridgeTargets;

// What the editor's requests act on. Non-owning; set once by bind().
struct EditorBridgeTargets {
    const engine::rhi::Device*   device = nullptr;
    engine::rhi::GpuAllocator*   allocator = nullptr;
    engine::asset::AssetManager* assets = nullptr;
    engine::scene::Scene*        scene = nullptr;
    WorldRuntime*                world = nullptr;
    FlyCamera*                   camera = nullptr;
    // Play/Stop (shared with the non-editor game; owned by the Application).
    PlayMode*                    play_mode = nullptr;
    // Shader hot-reload state for the Stats panel (R2); null when off.
    const engine::rhi::ShaderHotReload* shader_reload = nullptr;
    // The renderer's settings: the Renderer panel edits them and scene files
    // save / load them (A8).
    engine::renderer::RenderSettings* render_settings = nullptr;
    // Camera cut for the renderer's temporal state (R11): called after a scene
    // load and on every Play / Stop transition. May be empty.
    std::function<void()> reset_temporal;
    // Simulation clock (Pause / Step) and endpoint state for the status bar.
    RemoteBridge* remote = nullptr;
};

class EditorBridge {
public:
    EditorBridge() = default;
    EditorBridge(const EditorBridge&) = delete;
    EditorBridge& operator=(const EditorBridge&) = delete;
    EditorBridge(EditorBridge&&) = delete;
    EditorBridge& operator=(EditorBridge&&) = delete;

    // Creates the ImGui shell on `window`. Before the renderer's frame slots:
    // each slot's ImGuiPass uploads this context's font atlas. With `enabled`
    // false (QUAT_NO_UI=1) the editor stays inactive.
    void create(SDL_Window* window, bool enabled);
    void bind(const EditorBridgeTargets& targets) { t_ = targets; }
    // Points the remote bridge at the editor.
    void fill_remote_targets(RemoteBridgeTargets& targets);

    [[nodiscard]] bool active() const { return editor_.active(); }
    // True once after the editor rebuilt its font atlas (display scale
    // change); the frame renderer must re-upload it.
    [[nodiscard]] bool take_font_atlas_changed() { return editor_.take_font_atlas_changed(); }

    // ---- Input -------------------------------------------------------------
    void process_event(const SDL_Event& event);
    // Camera clicks belong to the Viewport panel; clicks anywhere else are
    // the UI's.
    [[nodiscard]] bool camera_accepts_clicks() const {
        return !(editor_.active() && !editor_.viewport_hovered());
    }
    // Editor mode: WASD flies only during a right-drag (W/E/R switch the
    // gizmo otherwise); the ARROW keys fly whenever the cursor is over the
    // viewport (they conflict with nothing). Text input freezes both.
    [[nodiscard]] FlyInputGate input_gate(bool looking, bool character_mode) const;
    // A UI text field owns the keyboard (scripts read keys as released).
    [[nodiscard]] bool keyboard_captured() const;
    // The scene's render size: the Viewport panel (sized by last frame's UI)
    // when the editor is up, else the whole window.
    [[nodiscard]] VkExtent2D scene_extent(VkExtent2D present_extent) const;

    // ---- Play mode ---------------------------------------------------------
    [[nodiscard]] bool playing() const {
        return t_.play_mode != nullptr && t_.play_mode->requested();
    }
    // This frame's play-mode simulation input (none outside play).
    [[nodiscard]] std::optional<PlayStep> play_step(const glm::vec3& fly_camera_pos) const;
    // Play viewed through the scene's active Camera entity (menu-bar "Game
    // camera"; falls back to the fly camera when none is active).
    [[nodiscard]] bool through_scene_camera() const;

    // ---- Frame hooks -------------------------------------------------------
    void begin_frame();
    void handle_ground_click();
    [[nodiscard]] const ImDrawData* build_ui(float dt, int draw_count,
                                             const engine::scene::CameraMatrices& cam);

private:
    void apply_scene_file_requests();
    void apply_instantiate_request();

    EditorBridgeTargets              t_;
    engine::editor::EditorLayer      editor_;
    std::string startup_select_;      // QUAT_EDITOR_SELECT, applied on the first frame
    std::string pending_instantiate_; // set by the asset browser, consumed below
    bool        new_scene_ = false;   // File > New
    std::string pending_save_;        // scene file requests from the File menu
    std::string pending_load_;
    bool play_use_scene_camera_ = false; // menu-bar "Game camera" checkbox
    std::optional<engine::editor::EditorContext::GroundClick> ground_click_; // 13.4
};

} // namespace quat::app

#endif // ENGINE_EDITOR
