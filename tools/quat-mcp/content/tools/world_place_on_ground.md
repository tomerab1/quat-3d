Set an entity (with its children) down on what is below it, so the bottom of its bounds rests on the surface: no guessing sink depths, no floating or buried models. Works for glTF models of any size, primitives and groups.

- `entity`: id or unique name.
- `at` `[x, z]`: move it there horizontally first (optional).
- `align_to_normal` (default false): tilt it so its up axis follows the surface normal under its centre (rocks, logs, props on slopes), limited to `max_tilt_deg` (default 30). Its heading is kept. Leave it off for trees, buildings and anything that should stand upright.
- `offset` (metres, default 0): added along world up afterwards; negative sinks it into the ground (e.g. -0.1 to bury a rock's base), positive lifts it.

Surfaces are the terrain and other entities' colliders (and, in play mode, every physics body); the entity's own colliders are ignored. Only surfaces below the top of the entity's bounds count, so a lamp under a roof lands on the floor; lift the entity above a surface to place it on top. Upright placement samples a 3x3 grid under the footprint: on terrain the bounds rest on the lowest sample (slightly buried on the uphill side rather than floating), on colliders on the highest (a crate overhanging a table edge stays on the table).

A failed placement changes nothing (the entity keeps its transform). While a terrain is regenerating its heights are unknown, so placement on it fails until `terrain_status` reports `generating: false`.

While a physics world runs (play mode, physics demo scenes) only its bodies count as objects: a collider added in the same batch is not a body until the next frame, so place onto it in a later call. A dynamic rigid body that is placed is moved back by the simulation; place it outside play mode.

Returns `{id, position, world_bounds, moved_y, tilt_deg, surface: {kind, entity, y, normal}}`; `kind` is terrain, collider, body, character or static_world. Fails when nothing is below (no loaded terrain there and no collider): check `terrain_status`, or give the surface a `collider`. For many objects, batch the calls with `scene_batch` (method `world.place_on_ground`).
