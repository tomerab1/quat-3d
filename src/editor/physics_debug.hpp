#pragma once

// Physics debug overlay (Phase 9, Slice 9.7). Private editor header.
//
// Draws collision-shape wireframes from the ECS Collider components projected
// over the Viewport panel. vcpkg's Jolt is compiled without JPH_DEBUG_RENDERER,
// so the engine draws its own shape descriptors instead of implementing the
// Jolt interface against a library that cannot service it; contact points are
// deferred until a Jolt build with the debug renderer is used.

#include <glm/glm.hpp>

#include "engine/editor/editor.hpp" // PhysicsDebugState

namespace engine::scene {
class Scene;
}

namespace engine::editor {

// Draws the "Physics" window (toggles) and, when enabled, wireframes every
// matching Collider into the viewport rect via the ImGui foreground draw list.
void draw_physics_debug(scene::Scene& scene, const glm::mat4& view_proj,
                        const float viewport_rect[4], PhysicsDebugState& state);

} // namespace engine::editor
