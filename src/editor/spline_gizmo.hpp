#pragma once

// Spline editing in the viewport (ROADMAP G18.3). Private editor header.
//
// With an entity that has a `spline` selected, the curve and its control
// points are drawn over the viewport: drag a point to move it (in the
// horizontal plane at its height), Ctrl+click the curve to insert a point,
// Alt+click a point to delete it. In draw mode a left-drag sketches a new
// stroke (Shift held: straight segments) that the shared stroke-capture API
// (engine/spline/stroke.hpp) fits into the spline.

#include <string>

#include <entt/entity/fwd.hpp>
#include <glm/glm.hpp>
#include <imgui.h>

#include "engine/scene/spline.hpp"
#include "engine/spline/stroke.hpp"

namespace engine::scene {
class Scene;
}

namespace engine::editor {

struct SplineGizmoState {
    bool                   draw_mode = false;
    int                    drag_point = -1;   // control point being dragged
    float                  drag_plane_y = 0.0F;
    spline::StrokeCapture  capture;
    std::string            message;            // last fit error / hint
    ImVec2                 button_min{0, 0};    // the draw-mode button (last frame)
    ImVec2                 button_max{0, 0};
};

// Handles the viewport mouse for the selected spline and draws it. Returns
// true when it used the mouse this frame (selection picking must not run).
// `hovered`: the pointer is over the viewport image.
bool spline_gizmo(scene::Scene& scene, entt::entity selected, const glm::mat4& view_proj, const ImVec2& min,
                  const ImVec2& max, bool hovered, SplineGizmoState& state);

// The draw-mode toggle and hint line in the viewport's bottom-left corner
// (call after the viewport's other items).
void spline_gizmo_overlay(scene::Scene& scene, entt::entity selected, const ImVec2& min, const ImVec2& max,
                          SplineGizmoState& state);

// The gizmo's edits (exposed for tests). Insert: a point on the curve at
// `param` of `local` (the spline's own curve; a Bézier segment is split, its
// handles kept). Delete: point `index` (a Bézier anchor with its handles),
// refused when the spline would fall below its kind's minimum.
void spline_insert_point(scene::Spline& spline, const spline::Curve& local, float param);
void spline_delete_point(scene::Spline& spline, std::size_t index);

// Inspector rows of the `spline` component.
void draw_spline_inspector(const entt::registry& registry, entt::entity e, scene::Spline& spline);

} // namespace engine::editor
