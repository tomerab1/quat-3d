#pragma once

// EditorLayer — ImGui editor shell (Phase 9, Slice 9.1).
//
// Owns the ImGui context and the SDL3 platform backend (input/windowing only;
// rendering is the engine's ImGuiPass). Builds the docking shell each frame:
// a fullscreen passthrough dockspace, the main menu bar, and the first panels.
// Editor state lives here, not in the ECS (CLAUDE.md). ImGui types stay out of
// this header — sources under src/editor/ are the only ImGui includes.

#include <expected>

#include "engine/core/error.hpp"

struct SDL_Window;
union SDL_Event;
struct ImDrawData;

namespace engine::editor {

class EditorLayer {
public:
    // Per-frame numbers surfaced in the Stats panel.
    struct FrameStats {
        float frame_ms     = 0.0F;
        int   draw_count   = 0;
        int   entity_count = 0;
    };

    // Creates the ImGui context (docking enabled) and the SDL3 platform
    // backend for `window`. One EditorLayer per process.
    [[nodiscard]] static std::expected<EditorLayer, core::Error> create(SDL_Window* window);

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

    // Frame flow: begin_frame() right after event processing, build_ui() once
    // the frame's stats are known, end_frame() to obtain the draw data for
    // ImGuiPass::add_to_graph.
    void begin_frame();
    void build_ui(const FrameStats& stats);
    [[nodiscard]] const ImDrawData* end_frame();

private:
    void destroy() noexcept;

    bool initialized_ = false;
    bool show_stats_  = true;
    bool show_demo_   = false;
};

} // namespace engine::editor
