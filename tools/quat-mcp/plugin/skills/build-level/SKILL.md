---
name: build-level
description: Build or extend a 3D level in a running quat-3d engine through the quat-mcp tools - terrain, primitives, glTF and Poly Haven models, forests and grass, lights, sky and particle effects - planned in metres, built in batches and checked with screenshots. Use when asked to build, block out, dress or extend a level, world, map or scene in quat-3d.
compatibility: Needs the quat-mcp MCP server connected to a running quat-3d engine (started with QUAT_REMOTE).
metadata:
  quat-mcp-tools: "engine_info scene_components scene_list scene_get scene_batch scene_update scene_save environment_get environment_set terrain_status world_ground_height world_place_on_ground asset_list_models asset_search asset_fetch asset_spawn_model camera_get camera_frame camera_set screenshot nav_bake"
---

# Build a level

Turn a description into a level in the running engine: inspect, plan with
coordinates, build in few round trips, look at the result, fix, look again.
Units are metres, seconds and degrees; Y is up.

## Workflow

1. **Inspect.** `engine_info` (build in edit mode: if play mode is running,
   whatever you build is undone when it stops), `scene_components` (which
   components exist and can be added), `scene_list` (what is already there),
   `environment_get` and `camera_get`.
   - Terrain: check `terrain_status` and wait while `generating` is true.
     Query `world_ground_height` for the spots you will build on.
   - Models: `asset_list_models` lists what the project has; `asset_search`
     finds free CC0 models on Poly Haven and `asset_fetch` downloads one into
     the project. Mind `polycount` when you need many copies.

2. **Plan before creating anything.** Write a short plan:
   - the layout with coordinates (flat ground is y = 0 unless there is terrain);
   - for each object its name, shape or model, position, rotation, scale and colour;
   - lights, time of day and clouds.

   Give entities unique descriptive names and group parts under a parent
   (`House_1` with children `House_1_Walls`, `House_1_Roof`) so they move
   together.

3. **Build in few round trips.** `scene_batch` with many `scene.create`
   commands; `"@N"` in a `parent` or `entity` field is the id created by
   command N (0-based) of the same batch. Use `asset_spawn_model` for glTF
   models.
   - Primitives are about unit size and centred on their position: size them
     with `transform.scale` and set position y to half the height so they
     rest on the ground.
   - Models and props on uneven ground: `world_place_on_ground` (it reads
     their bounds) instead of guessing heights. Batch many placements with
     `scene_batch` (method `world.place_on_ground`).
   - Ground is a `terrain`, never a flat primitive with a colour (that reads
     as a plastic sheet). Its `material` gives a textured, rule-blended
     ground by default (`temperate_meadow`); pick the preset that fits
     (`alpine`, `desert`, `forest_floor`). For a small scene (a camp, a
     yard) keep it gentle: `{"tile_size": 256, "resolution": 257, "height":
     6, "frequency": 1.5, "erosion": "grid", "erosion_steps": 60}` gives
     soft hummocks over 256 m; the terrain's centre sits at its entity
     position.
   - Repeated objects (forests, rock fields) are one entity with an
     `instanced_mesh` plus a `scatter`, never one entity per copy. Ground
     cover is one entity with a `grass` component.
   - Physics only where it matters: `collider` + `rigidbody` (a collider
     alone becomes a static body in play mode).

4. **Set the mood.** `environment_set` for the sun (`elevation_deg` 5..10 for
   golden hour, 45..70 for daylight) and clouds. Lamps, fires and windows are
   entities with a `point_light` (warm linear colours such as
   [1.0, 0.6, 0.3]).
   - VFX (fire, sparks, smoke, steam, dust, falling leaves) are one entity
     each with a `particle_emitter`, placed where the effect starts:
     `blend` additive with `emissive` > 1 for anything that glows; `blend`
     alpha, `lighting` lit and a slight upward `gravity` for smoke. Pair a
     fire with a warm point light. Keep `max_particles` to hundreds.

5. **Look.** `screenshot` with a `camera`
   (`{"camera": {"frame": "Watchtower"}}`, or a `position` and `look_at`).
   It leaves the user's viewport alone; use `camera_frame` or `camera_set`
   only when the user should see the view. Take a second shot from another
   angle for anything large.

6. **Iterate.** Compare the image with the goal: proportions, gaps, floating
   or buried objects, overlaps, colours, lighting, composition. Fix with
   `scene_update` (it merges: give only the fields that change) or another
   `scene_batch`, then screenshot again.

7. **Finish.** If NPCs will walk the level, `nav_bake` after the static
   geometry is final. Summarise what you built; `scene_save` only if asked.

## Verify

- Every object you placed is visible in a screenshot from at least one
  angle, at the intended size.
- `scene_get` on a model reports `world_bounds`: its bottom (`min[1]`)
  matches the ground height there, so nothing floats or sinks.
- `scene_list` shows the hierarchy you planned, with no duplicate or empty
  names.

## Pitfalls

- Rotation is Euler degrees [x pitch, y yaw, z roll] applied in YXZ order;
  an entity's forward is -Z. Scale is a number or [x, y, z].
- Colours: primitives take `"#rrggbb"` sRGB hex; lights take linear
  [r, g, b].
- A `scene_batch` stops at the first failed command by default; the commands
  before it stay applied. Read `results`, fix the failing command and send
  only the rest.
- After creating or reshaping terrain (new `seed`, `height`, `tile_size`, ...)
  heights are unknown until `terrain_status` reports `generating: false`;
  placement fails meanwhile. Shading edits such as `snowline` apply at once.
- Collider `half_extents` are in the entity's own unscaled frame and are
  multiplied by its scale: a scaled unit cube still takes [0.5, 0.5, 0.5].
- Screenshots taken far from the viewport camera may miss streamed terrain
  tiles; check `terrain_status` or move the viewport there first.
- Engine errors name the bad field and what was expected: correct the call
  instead of repeating it.
