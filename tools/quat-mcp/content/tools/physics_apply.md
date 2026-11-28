Push a dynamic rigid body. Give exactly one of:

- `impulse` (N·s): an instant kick. The velocity changes at once by impulse / mass, so `[0, 5, 0]` on a 1 kg crate launches it up at 5 m/s.
- `force` (N): applied every simulation step for `duration_s` seconds of simulated time (default 1/60, at most 10; it must round to at least one 1/120 s step). Forces act in whole fixed steps of 1/120 s, so the duration is rounded to the nearest step count; the reply's `duration_s` and `steps` say what was applied. Force × duration is the total impulse.
- `angular_impulse` (N·m·s): an instant spin about the world axes through the centre of mass.
- `torque` (N·m): a twist applied every step for `duration_s`.

`at_point` (world space, impulse or force only) pushes at that point, so off-centre pushes also spin the body.

Only dynamic bodies respond; static and kinematic bodies are an error that names their motion type. The reply is the body's state after the push (`physics_get` fields plus `applied`), so you can read the new velocity at once. Gravity is about 9.81 m/s² × mass downward: to lift a 2 kg box, use a force above 19.6 N.
