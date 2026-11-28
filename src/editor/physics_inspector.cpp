#include "physics_inspector.hpp"

#include <entt/entity/registry.hpp>
#include <glm/glm.hpp>

#include "engine/scene/components.hpp"
#include "ui/theme.hpp"
#include "ui/widgets.hpp"

namespace engine::editor {

void draw_collider_fields(scene::Collider& collider) {
    static constexpr ui::EnumItem shapes[] = {
        {static_cast<int>(scene::ColliderShape::box), "Box", nullptr},
        {static_cast<int>(scene::ColliderShape::sphere), "Sphere", nullptr},
        {static_cast<int>(scene::ColliderShape::capsule), "Capsule", nullptr},
    };
    (void)ui::prop_enum("Shape", collider.shape, shapes);
    const ui::DragSpec size{.speed = 0.05F, .min = 0.01F, .max = 1000.0F};
    switch (collider.shape) {
    case scene::ColliderShape::box:
        (void)ui::prop_vec3("Half extents", collider.half_extents, size);
        break;
    case scene::ColliderShape::sphere:
        (void)ui::prop_float("Radius", collider.half_extents.x, size);
        break;
    case scene::ColliderShape::capsule:
        (void)ui::prop_float("Radius", collider.half_extents.x, size);
        (void)ui::prop_float("Half height", collider.half_extents.y, size);
        break;
    }
    (void)ui::prop_vec3("Offset", collider.offset, {.speed = 0.05F}, {.reset = glm::vec3(0.0F)});
    (void)ui::prop_bool("Trigger", collider.is_sensor,
                        {.reset = false, .tooltip = "Overlap events only: objects pass through"});
    if (collider.is_sensor) {
        ui::prop_note("No collision response: objects pass through.", ui::tokens().warning);
    }
    ui::prop_note("Without a Rigid Body this simulates as static; add a dynamic Rigid Body "
                  "to make it fall in Play.");
}

void draw_rigid_body_fields(scene::RigidBody& body) {
    static constexpr ui::EnumItem motions[] = {
        {static_cast<int>(scene::BodyMotion::static_body), "Static", "Never moves"},
        {static_cast<int>(scene::BodyMotion::kinematic), "Kinematic", "Moved by the transform"},
        {static_cast<int>(scene::BodyMotion::dynamic), "Dynamic", "Simulated by Jolt"},
    };
    const scene::RigidBody defaults{};
    (void)ui::prop_enum("Motion", body.motion, motions);
    (void)ui::prop_float("Mass", body.mass,
                         {.speed = 0.1F, .min = 0.01F, .max = 10000.0F, .format = "%.2f kg"},
                         {.reset = defaults.mass});
    (void)ui::prop_float("Friction", body.friction, {.speed = 0.01F, .min = 0.0F, .max = 10.0F},
                         {.reset = defaults.friction, .tooltip = "Coulomb friction coefficient"});
    (void)ui::prop_float("Restitution", body.restitution,
                         {.speed = 0.01F, .min = 0.0F, .max = 1.0F, .clamp = true},
                         {.reset = defaults.restitution, .tooltip = "Bounciness, 0..1"});
    (void)ui::prop_float("Gravity factor", body.gravity_factor,
                         {.speed = 0.05F, .min = -10.0F, .max = 10.0F},
                         {.reset = defaults.gravity_factor,
                          .tooltip = "Scales the scene gravity for this body"});
    (void)ui::prop_float("Linear damping", body.linear_damping,
                         {.speed = 0.01F, .min = 0.0F, .max = 100.0F, .format = "%.3f /s"},
                         {.reset = defaults.linear_damping});
    (void)ui::prop_float("Angular damping", body.angular_damping,
                         {.speed = 0.01F, .min = 0.0F, .max = 100.0F, .format = "%.3f /s"},
                         {.reset = defaults.angular_damping});
    ui::prop_value("Body", body.body == 0xFFFFFFFFU ? "not created (press Play)" : "live");
}

void draw_joint_fields(const entt::registry& r, scene::Joint& joint) {
    static constexpr ui::EnumItem types[] = {
        {static_cast<int>(scene::JointType::fixed), "Fixed", "Glues the bodies in their pose"},
        {static_cast<int>(scene::JointType::point), "Point", "Ball-and-socket at the anchor"},
        {static_cast<int>(scene::JointType::hinge), "Hinge", "Rotation about the axis"},
        {static_cast<int>(scene::JointType::slider), "Slider", "Translation along the axis"},
        {static_cast<int>(scene::JointType::distance), "Distance", "Rope / rod between anchors"},
    };
    static constexpr ui::EnumItem motors[] = {
        {static_cast<int>(scene::JointMotorMode::off), "Off", nullptr},
        {static_cast<int>(scene::JointMotorMode::velocity), "Velocity", nullptr},
        {static_cast<int>(scene::JointMotorMode::position), "Position", nullptr},
    };
    bool changed = false;
    if (ui::prop_enum("Type", joint.type, types)) {
        joint.limits_enabled = false; // limits mean different things per type
        joint.motor = scene::JointMotorMode::off;
        changed = true;
    }
    const auto* target = joint.target != entt::null && r.valid(joint.target)
                             ? r.try_get<scene::Name>(joint.target)
                             : nullptr;
    ui::prop_value("Target", joint.target == entt::null ? "(world)"
                             : target != nullptr        ? target->value
                                                        : "(unnamed entity)");
    changed |= ui::prop_vec3("Anchor", joint.anchor, {}, {.reset = glm::vec3(0.0F)});
    const bool hinge = joint.type == scene::JointType::hinge;
    const bool slider = joint.type == scene::JointType::slider;
    const bool distance = joint.type == scene::JointType::distance;
    if (hinge || slider) {
        changed |= ui::prop_vec3("Axis", joint.axis, {.min = -1.0F, .max = 1.0F});
    }
    if (distance) {
        changed |= ui::prop_vec3("Target anchor", joint.target_anchor, {},
                                 {.reset = glm::vec3(0.0F)});
    }
    if (hinge || slider || distance) {
        changed |= ui::prop_bool("Limits", joint.limits_enabled, {.reset = false});
        if (joint.limits_enabled) {
            changed |= ui::prop_range(hinge ? "Min / max (deg)" : "Min / max (m)", joint.limits,
                                      {.speed = hinge ? 0.5F : 0.01F});
        }
    }
    if (hinge || slider) {
        changed |= ui::prop_enum("Motor", joint.motor, motors);
        if (joint.motor != scene::JointMotorMode::off) {
            changed |= ui::prop_float("Motor target", joint.motor_target, {.speed = 0.1F});
            changed |= ui::prop_float("Max force", joint.motor_max_force,
                                      {.speed = 1.0F, .min = 0.0F, .max = 1.0e9F},
                                      {.reset = 0.0F, .tooltip = "0 = unlimited"});
        }
    }
    if (changed) joint.constraint = 0xFFFFFFFFU; // rebuilt from the edit next tick
    ui::prop_value("Constraint",
                   joint.constraint == 0xFFFFFFFFU ? "not built (press Play)" : "live");
}

} // namespace engine::editor
