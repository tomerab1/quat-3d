//! Inputs of the physics-control tools (`physics_*`, `events_poll`,
//! `character_move`; docs/remote-control.md "Physics control"). The engine
//! validates every value; these types give the model the shape and units.

use schemars::JsonSchema;
use serde::{Deserialize, Serialize};

use super::EntityRef;

/// A world-space vector `[x, y, z]` (Y is up).
pub type Vec3 = [f64; 3];

/// A kind of gameplay event.
#[derive(Debug, Clone, Copy, PartialEq, Eq, Serialize, Deserialize, JsonSchema)]
#[serde(rename_all = "snake_case")]
pub enum EventKind {
    ContactAdded,
    ContactRemoved,
    TriggerEnter,
    TriggerExit,
}

#[derive(Debug, Clone, Default, Serialize, Deserialize, JsonSchema)]
#[serde(deny_unknown_fields)]
pub struct EventsPollParams {
    /// Return only events after this sequence number: pass the `cursor` of the
    /// previous call. Omit (or 0) for every retained event.
    #[serde(default, skip_serializing_if = "Option::is_none")]
    pub since: Option<u64>,
    /// Most events to return, 1..2000 (default 200).
    #[serde(default, skip_serializing_if = "Option::is_none")]
    pub max: Option<u32>,
    /// Only these kinds of event.
    #[serde(default, skip_serializing_if = "Option::is_none")]
    pub kinds: Option<Vec<EventKind>>,
    /// Only events involving this entity (id or unique name).
    #[serde(default, skip_serializing_if = "Option::is_none")]
    pub entity: Option<EntityRef>,
}

/// How a body moves.
#[derive(Debug, Clone, Copy, PartialEq, Eq, Serialize, Deserialize, JsonSchema)]
#[serde(rename_all = "lowercase")]
pub enum MotionType {
    Static,
    Kinematic,
    Dynamic,
}

#[derive(Debug, Clone, Serialize, Deserialize, JsonSchema)]
#[serde(deny_unknown_fields)]
pub struct PhysicsSetParams {
    /// The entity with the rigid body: integer id (preferred) or exact unique name.
    pub entity: EntityRef,
    /// New linear velocity in m/s (dynamic bodies only).
    #[serde(default, skip_serializing_if = "Option::is_none")]
    pub linear_velocity: Option<Vec3>,
    /// New angular velocity in degrees per second about the world axes (dynamic bodies only).
    #[serde(default, skip_serializing_if = "Option::is_none")]
    pub angular_velocity: Option<Vec3>,
    /// Switch between static, kinematic and dynamic. To or from static rebuilds the body next frame.
    #[serde(default, skip_serializing_if = "Option::is_none")]
    pub motion_type: Option<MotionType>,
    /// Multiplies the scene gravity for this body, -10..10 (0 = floats).
    #[serde(default, skip_serializing_if = "Option::is_none")]
    pub gravity_factor: Option<f64>,
    /// Friction coefficient, 0..10 (default 0.2).
    #[serde(default, skip_serializing_if = "Option::is_none")]
    pub friction: Option<f64>,
    /// Bounciness, 0..1.
    #[serde(default, skip_serializing_if = "Option::is_none")]
    pub restitution: Option<f64>,
    /// Linear damping in 1/s, 0..100 (default 0.05).
    #[serde(default, skip_serializing_if = "Option::is_none")]
    pub linear_damping: Option<f64>,
    /// Angular damping in 1/s, 0..100 (default 0.05).
    #[serde(default, skip_serializing_if = "Option::is_none")]
    pub angular_damping: Option<f64>,
    /// true wakes the body, false puts it to sleep.
    #[serde(default, skip_serializing_if = "Option::is_none")]
    pub wake: Option<bool>,
}

impl PhysicsSetParams {
    /// Whether the call changes anything besides naming the entity.
    #[must_use]
    pub fn is_empty(&self) -> bool {
        self.linear_velocity.is_none()
            && self.angular_velocity.is_none()
            && self.motion_type.is_none()
            && self.gravity_factor.is_none()
            && self.friction.is_none()
            && self.restitution.is_none()
            && self.linear_damping.is_none()
            && self.angular_damping.is_none()
            && self.wake.is_none()
    }
}

#[derive(Debug, Clone, Serialize, Deserialize, JsonSchema)]
#[serde(deny_unknown_fields)]
pub struct PhysicsApplyParams {
    /// The entity with the dynamic rigid body: integer id (preferred) or exact unique name.
    pub entity: EntityRef,
    /// Instant push in N*s (kg*m/s): changes the velocity by impulse / mass at once.
    #[serde(default, skip_serializing_if = "Option::is_none")]
    pub impulse: Option<Vec3>,
    /// Push in newtons, applied every simulation step for `duration_s`.
    #[serde(default, skip_serializing_if = "Option::is_none")]
    pub force: Option<Vec3>,
    /// Instant spin in N*m*s about the world axes through the centre of mass.
    #[serde(default, skip_serializing_if = "Option::is_none")]
    pub angular_impulse: Option<Vec3>,
    /// Twist in N*m, applied every simulation step for `duration_s`.
    #[serde(default, skip_serializing_if = "Option::is_none")]
    pub torque: Option<Vec3>,
    /// World-space point to push at (impulse or force only): off-centre pushes also spin.
    #[serde(default, skip_serializing_if = "Option::is_none")]
    pub at_point: Option<Vec3>,
    /// Seconds of simulated time a force or torque acts, 1/120..10 (default 1/60), in whole 1/120 s steps.
    #[serde(default, skip_serializing_if = "Option::is_none")]
    pub duration_s: Option<f64>,
}

impl PhysicsApplyParams {
    /// How many of impulse / force / `angular_impulse` / torque are given.
    #[must_use]
    pub fn kinds(&self) -> usize {
        [
            self.impulse.is_some(),
            self.force.is_some(),
            self.angular_impulse.is_some(),
            self.torque.is_some(),
        ]
        .into_iter()
        .filter(|given| *given)
        .count()
    }
}

#[derive(Debug, Clone, Serialize, Deserialize, JsonSchema)]
#[serde(deny_unknown_fields)]
pub struct GravityParams {
    /// Gravity in m/s^2, world space; Earth is [0, -9.81, 0], the Moon [0, -1.62, 0].
    pub gravity: Vec3,
}

/// A collision layer.
#[derive(Debug, Clone, Copy, PartialEq, Eq, Serialize, Deserialize, JsonSchema)]
#[serde(rename_all = "lowercase")]
pub enum Layer {
    Static,
    Dynamic,
    Character,
    Trigger,
    Sensor,
}

#[derive(Debug, Clone, Serialize, Deserialize, JsonSchema)]
#[serde(deny_unknown_fields)]
pub struct RaycastAllParams {
    /// Ray start, world space.
    pub origin: Vec3,
    /// Ray direction; need not be normalized, must not be zero.
    pub direction: Vec3,
    /// Longest distance to test, metres (default 1000).
    #[serde(default, skip_serializing_if = "Option::is_none")]
    pub max_distance: Option<f64>,
    /// Layers to hit (default static, dynamic, character; add sensor for trigger volumes).
    #[serde(default, skip_serializing_if = "Option::is_none")]
    pub layers: Option<Vec<Layer>>,
    /// Most hits to return, 1..256 (default 32).
    #[serde(default, skip_serializing_if = "Option::is_none")]
    pub max_hits: Option<u32>,
}

/// A query shape.
#[derive(Debug, Clone, Copy, PartialEq, Eq, Serialize, Deserialize, JsonSchema)]
#[serde(rename_all = "lowercase")]
pub enum QueryShape {
    Sphere,
    Box,
    Capsule,
}

#[derive(Debug, Clone, Serialize, Deserialize, JsonSchema)]
#[serde(deny_unknown_fields)]
pub struct OverlapParams {
    /// The shape to test.
    pub shape: QueryShape,
    /// Sphere or capsule radius, metres (default 0.5).
    #[serde(default, skip_serializing_if = "Option::is_none")]
    pub radius: Option<f64>,
    /// Box half size per axis, metres (default [0.5, 0.5, 0.5]).
    #[serde(default, skip_serializing_if = "Option::is_none")]
    pub half_extents: Option<Vec3>,
    /// Capsule: half the length of its straight part along local Y, metres (default 0.5).
    #[serde(default, skip_serializing_if = "Option::is_none")]
    pub half_height: Option<f64>,
    /// Centre of the shape, world space.
    pub position: Vec3,
    /// Euler angles in degrees [pitch, yaw, roll], as `transform.rotation`.
    #[serde(default, skip_serializing_if = "Option::is_none")]
    pub rotation: Option<Vec3>,
    /// Layers to report (default static, dynamic, character).
    #[serde(default, skip_serializing_if = "Option::is_none")]
    pub layers: Option<Vec<Layer>>,
    /// Most hits to return, 1..256 (default 32).
    #[serde(default, skip_serializing_if = "Option::is_none")]
    pub max_hits: Option<u32>,
}

#[derive(Debug, Clone, Serialize, Deserialize, JsonSchema)]
#[serde(deny_unknown_fields)]
pub struct ShapeCastParams {
    /// The shape to sweep.
    pub shape: QueryShape,
    /// Sphere or capsule radius, metres (default 0.5).
    #[serde(default, skip_serializing_if = "Option::is_none")]
    pub radius: Option<f64>,
    /// Box half size per axis, metres (default [0.5, 0.5, 0.5]).
    #[serde(default, skip_serializing_if = "Option::is_none")]
    pub half_extents: Option<Vec3>,
    /// Capsule: half the length of its straight part along local Y, metres (default 0.5).
    #[serde(default, skip_serializing_if = "Option::is_none")]
    pub half_height: Option<f64>,
    /// Start of the sweep (shape centre), world space.
    pub from: Vec3,
    /// End of the sweep (shape centre), world space; must differ from `from`.
    pub to: Vec3,
    /// Euler angles in degrees [pitch, yaw, roll], fixed during the sweep.
    #[serde(default, skip_serializing_if = "Option::is_none")]
    pub rotation: Option<Vec3>,
    /// Layers to hit (default static, dynamic, character).
    #[serde(default, skip_serializing_if = "Option::is_none")]
    pub layers: Option<Vec<Layer>>,
    /// Most hits to return, 1..256 (default 32).
    #[serde(default, skip_serializing_if = "Option::is_none")]
    pub max_hits: Option<u32>,
}

/// A jump: a speed in m/s, or `true` for the default 5 m/s.
#[derive(Debug, Clone, Copy, PartialEq, Serialize, Deserialize, JsonSchema)]
#[serde(untagged)]
pub enum Jump {
    Default(bool),
    Speed(f64),
}

#[derive(Debug, Clone, Serialize, Deserialize, JsonSchema)]
#[serde(deny_unknown_fields)]
pub struct CharacterMoveParams {
    /// The entity with the character controller: integer id (preferred) or exact unique name.
    pub entity: EntityRef,
    /// Walk velocity in m/s; only x and z count, at most the controller's `speed`.
    /// [0, 0, 0] stops.
    #[serde(default, skip_serializing_if = "Option::is_none")]
    pub velocity: Option<Vec3>,
    /// Walk in a straight line to this world point at full speed and stop there.
    #[serde(default, skip_serializing_if = "Option::is_none")]
    pub target: Option<Vec3>,
    /// Jump when on the ground: a take-off speed in m/s, or true for 5 m/s.
    #[serde(default, skip_serializing_if = "Option::is_none")]
    pub jump: Option<Jump>,
    /// Stop walking after this many seconds of simulated time, 0.01..60 (default: keep walking).
    #[serde(default, skip_serializing_if = "Option::is_none")]
    pub duration_s: Option<f64>,
}

#[cfg(test)]
mod tests {
    use super::*;
    use serde_json::json;

    #[test]
    fn events_poll_params_serialize_only_what_is_given() {
        let p: EventsPollParams =
            serde_json::from_value(json!({"since": 7, "kinds": ["trigger_enter"]})).unwrap();
        assert_eq!(
            serde_json::to_value(&p).unwrap(),
            json!({"since": 7, "kinds": ["trigger_enter"]})
        );
        assert!(serde_json::from_value::<EventsPollParams>(json!({"kinds": ["boom"]})).is_err());
        assert!(serde_json::from_value::<EventsPollParams>(json!({"cursor": 1})).is_err());
    }

    #[test]
    fn apply_params_count_their_kinds() {
        let p: PhysicsApplyParams =
            serde_json::from_value(json!({"entity": "Crate", "impulse": [0, 5, 0]})).unwrap();
        assert_eq!(p.kinds(), 1);
        let p: PhysicsApplyParams =
            serde_json::from_value(json!({"entity": 3, "force": [1, 0, 0], "torque": [0, 1, 0]}))
                .unwrap();
        assert_eq!(p.kinds(), 2);
    }

    #[test]
    fn set_params_know_when_they_are_empty() {
        let p: PhysicsSetParams = serde_json::from_value(json!({"entity": 1})).unwrap();
        assert!(p.is_empty());
        let p: PhysicsSetParams =
            serde_json::from_value(json!({"entity": 1, "motion_type": "kinematic"})).unwrap();
        assert!(!p.is_empty());
        assert!(
            serde_json::from_value::<PhysicsSetParams>(json!({"entity": 1, "motion": "x"}))
                .is_err()
        );
    }

    #[test]
    fn jumps_are_a_flag_or_a_speed() {
        let p: CharacterMoveParams =
            serde_json::from_value(json!({"entity": "Hero", "jump": true})).unwrap();
        assert_eq!(p.jump, Some(Jump::Default(true)));
        let p: CharacterMoveParams =
            serde_json::from_value(json!({"entity": "Hero", "jump": 7.5})).unwrap();
        assert_eq!(p.jump, Some(Jump::Speed(7.5)));
        assert_eq!(serde_json::to_value(&p).unwrap()["jump"], json!(7.5));
    }
}
