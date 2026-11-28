Read an entity's live physics state while the simulation runs (`sim_play` first).

For a rigid body (an entity with `collider` + `rigidbody`) it returns `{kind: "body", motion_type, layer, sensor, sleeping, mass, linear_velocity (m/s), angular_velocity (degrees/s), speed, position (body centre), friction, restitution, gravity_factor, linear_damping, angular_damping}`. For a character controller it returns `{kind: "character", position, velocity, on_ground, move, speed}`.

Use it to check whether a body is moving, asleep, or as heavy as expected before and after `physics_apply` or `physics_set`. A body that was just created or edited is built on the next simulated frame. Until then the call reports `unavailable`; step once with `debug_step` if the simulation is paused.
