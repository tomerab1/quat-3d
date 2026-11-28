Change a running rigid body. Give any of:

- `linear_velocity` (m/s) and `angular_velocity` (degrees/s): dynamic bodies only.
- `motion_type`: `static`, `kinematic` or `dynamic`. Switching to or from `static` rebuilds the body on the next frame (the reply says `rebuilt: true`); set velocities in a later call.
- `gravity_factor` (-10..10; 0 floats, negative rises), `friction` (0..10), `restitution` (0..1, bounciness), `linear_damping` and `angular_damping` (0..100, 1/s).
- `wake`: true wakes a sleeping body, false puts it to sleep.

The call is atomic: a bad field changes nothing and the error names it. It returns the body's state as `physics_get` does.

Tuning values (`motion_type`, `gravity_factor`, `friction`, `restitution`, damping) are also written to the entity's `rigidbody` component, so `scene_get` shows them. They last until `sim_stop` restores the scene. To make them permanent, set them with `scene_update` outside play mode.
