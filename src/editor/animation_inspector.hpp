#pragma once

// Inspector editors for the animation components (G8). Private editor
// header; inspector.cpp's ComponentInspector specialisations delegate here.
// Both draw property-grid rows (call inside ui::begin_property_grid); Foot
// IK's enable toggle sits in its component header.

#include <entt/entity/fwd.hpp>

namespace engine::scene {
struct Animator;
struct FootIK;
} // namespace engine::scene

namespace engine::editor {

// Clip picker (switching uses the transition settings), blend mode + time,
// speed and looping.
void draw_animator(scene::Animator& animator);

// Joint names (empty = auto-detect), probe and limit settings, and the
// resolved rig / live offsets from the animation runtime.
void draw_foot_ik(entt::registry& registry, entt::entity entity, scene::FootIK& foot_ik);

} // namespace engine::editor
