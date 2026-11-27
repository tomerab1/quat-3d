## Components

A JSON object keyed by component name. Updates merge: omitted fields keep their values. Unknown names/fields are rejected with the list of known ones. Units: metres, seconds, degrees; Y is up.

- **transform**: `position` [x,y,z] (local, relative to the parent); `rotation` [x,y,z] Euler degrees applied in YXZ order (x = pitch, y = yaw/heading, z = roll); `scale` number or [x,y,z]; `look_at` [x,y,z] world point to face (an entity's forward is -Z). `world_position` is read-only.
- **primitive**: `shape` cube|sphere|plane|cylinder|cone|capsule (about unit size, centred on the position — size it with transform.scale; a cube scaled [w,h,d] rests on y=0 at position y=h/2); `color` `"#rrggbb"` / `"#rrggbbaa"` sRGB hex or linear [r,g,b,a] (alpha < 1 = see-through); `metallic` 0..1; `roughness` 0..1; `emissive` linear [r,g,b] (HDR, > 1 glows); `shader` a material shader name (`shader_list`; `"standard"` = built-in PBR) — the shader computes the surface from these fields and `shader_params` (up to 8 numbers, its free knobs); with a shader the object stays opaque.
- **`point_light`**: `color` linear [r,g,b]; `radius` (range, m); `intensity` >= 0.
- **`dir_light`**: `direction` [x,y,z] (the light's travel direction); `color` linear [r,g,b]; `intensity`. Prefer `environment_set` for the sun.
- **camera**: `fov_deg` 1..179; `near`; `far` (> near); `active`.
- **collider**: `shape` box|sphere|capsule; `half_extents` [x,y,z] (> 0); `offset` [x,y,z]; `sensor` (trigger volume, no collision response).
- **rigidbody**: `motion` static|kinematic|dynamic; `mass` (kg). Pair with a collider.
- **character**: `radius`; `half_height`; `speed` (m/s) — capsule character controller.
- **nav_agent**: `target` [x,y,z]; `speed` (m/s); `ground_offset`; `active`. Needs a baked navmesh (`nav_bake`).
- **behavior**: `source` (behaviour-tree JSON object or its text); `enabled`.
- **patrol**: `points` [[x,y,z],...]; `loop`. Walked by the entity's `nav_agent`.
- **terrain**: `seed`; `resolution` 17..8193; `tile_size` (m); `height` (m); `octaves` 1..16; `warp` 0..4; `droplets` (erosion, 0..50000000); `snowline` (m); `streaming`; `stream_radius` 0..8 (tiles).
- **`instanced_mesh`**: many copies of one model on ONE entity (forests, rocks) — prefer it over many entities. `gltf` (path relative to the project root; `node` = only that node's subtree) or, with gltf empty, the primitive fields `shape`/`color`/`metallic`/`roughness`/`emissive`/`shader`/`shader_params`; `cast_shadows`; `instances` [{position, rotation, scale} or [x,y,z], ...] relative to the entity (omitted while a scatter generates them); `instance_count` is read-only.
- **`scatter`**: procedural placement of the entity's `instanced_mesh` on the terrain around the entity: `seed`; `area` rect|circle; `size` [x,z] (m); `radius` (m); `density` (per m²); `min_spacing` (m); `pattern` `poisson`|`jittered_grid`; `scale` [min,max]; `random_yaw`; `align_to_normal` 0..1; `base_offset` (× scale; 0.5 seats a unit primitive on the ground); `slope` [min,max] degrees; `height` [min,max] m; `max_instances`. Same seed = same placements.
- **`net_replicated`**: `net_id`.
- **animator** (edit only, present on animated glTF models): `time` (s); `speed`; `looping`.
- **mesh**: read only (glTF provenance).

`scene_components` lists what the running engine supports.
