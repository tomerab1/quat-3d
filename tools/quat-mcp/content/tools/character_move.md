Drive a character controller (an entity with the `character` component) the way a player would, while the simulation runs:

- `velocity` `[x, y, z]` m/s: walk in that direction. Only x and z count, and the speed must not exceed the controller's `speed`. `[0, 0, 0]` stops.
- `target` `[x, y, z]`: walk straight to that point at full speed and stop within a few centimetres. The walk follows no path and goes around nothing; for routes around obstacles, give the entity a `nav_agent` and set its target.
- `jump`: a take-off speed in m/s (up to 50), or `true` for 5 m/s. It only happens when the character is on the ground.
- `duration_s`: stop walking after this much simulated time (default: keep walking until told otherwise).

Give `velocity` or `target` (not both), optionally with `jump`; `jump` alone also works. The character collides with the world, bodies and other characters, and gravity pulls it down. It replies `{position, velocity, on_ground, move, target?}`. Follow progress with `physics_get` or `debug_watch`.

A character that also has an active `nav_agent` is steered by navigation, so this call is refused until the agent is stopped.
