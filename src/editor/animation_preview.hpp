#pragma once

// Animation preview panel (Phase 9, Slice 9.6). Private editor header.

#include <entt/entity/entity.hpp>
#include <glm/glm.hpp>

#include "engine/editor/editor.hpp" // AnimationPreviewState

namespace engine::scene {
class Scene;
}

namespace engine::editor {

// Draws the "Animation" window for the selected entity's Animator (timeline
// scrubber, play/pause, loop, speed) and — when enabled — overlays the
// skeleton's joint hierarchy over the viewport (`viewport_rect` = panel image
// min/max in screen pixels, `view_proj` = the scene camera's matrix).
void draw_animation_preview(scene::Scene& scene, entt::entity selected,
                            const glm::mat4& view_proj, const float viewport_rect[4],
                            AnimationPreviewState& state);

} // namespace engine::editor
