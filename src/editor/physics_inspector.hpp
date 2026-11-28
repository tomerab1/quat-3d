#pragma once

// Editor UI for the physics components (Collider, Rigid Body, Joint).
// Private editor header: inspector.cpp's ComponentInspector specialisations
// delegate here, keeping that file within the size rule. Each function
// draws property-grid rows; the inspector draws the section header.

#include <entt/entity/fwd.hpp>

namespace engine::scene {
struct Collider;
struct RigidBody;
struct Joint;
} // namespace engine::scene

namespace engine::editor {

void draw_collider_fields(scene::Collider& collider);
// Motion, mass, the surface / motion tuning (A10.1) and whether the body is live.
void draw_rigid_body_fields(scene::RigidBody& body);
// Type, target, anchors / axis, limits and motor. An edit marks the
// constraint for a rebuild on the next tick.
void draw_joint_fields(const entt::registry& registry, scene::Joint& joint);

} // namespace engine::editor
