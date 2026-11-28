---
name: debug-physics-glitch
description: Diagnose physics glitches in a running quat-3d game - objects falling through floors or terrain, jittering or exploding at start, floating, tunnelling through walls, not falling, characters stuck, triggers that never fire - by pausing and single-stepping the simulation, watching bodies frame by frame and casting rays. Use when something in quat-3d moves, collides or rests wrongly.
compatibility: Needs the quat-mcp MCP server connected to a running quat-3d engine (started with QUAT_REMOTE).
metadata:
  quat-mcp-tools: "engine_info debug_stats debug_log scene_list scene_get scene_update world_place_on_ground terrain_status sim_play sim_stop debug_pause debug_step debug_resume debug_watch debug_raycast debug_time_scale screenshot"
---

# Debug a physics glitch

Observe, form a hypothesis, test it - do not change the scene until you know
the cause. Physics bodies exist only in play mode: they are created from
each entity's `collider` + `rigidbody` at its world transform when Play
starts (or when the components are added while playing), and `sim_stop`
restores the scene as it was before Play.

## How bodies are built

- `rigidbody` `motion`: `dynamic` (physics moves the entity), `kinematic`
  (the entity's transform moves the body), `static` (never moves). A
  `collider` without a `rigidbody` becomes a static body.
- `collider` `half_extents` are in the entity's own unscaled frame and are
  multiplied by its world scale: box half extents per axis; sphere radius =
  x times the largest scale; capsule radius = x times the larger of the x and
  z scale, half-height = y times the y scale. A unit primitive cube or sphere
  therefore takes [0.5, 0.5, 0.5] whatever its scale. `offset` shifts the
  shape in the entity's scaled, rotated frame.
- Terrain is a static height field (`kind: static_world` in ray hits).
- `character` entities are capsule controllers, not rigid bodies.

## Workflow

1. **Health check.** `engine_info` (is play mode running?) and
   `debug_stats`: `physics` bodies (`null` outside play mode), frame times
   (a `worst` far above `avg` means hitches, which make fast bodies jump),
   and log error counts. `debug_log` with `{"level": "warn"}`.
2. **Find the subjects.** `scene_list` with `component: "rigidbody"` (and
   `"collider"`), then `scene_get` on the bodies involved and on what they
   should rest on. Compare each collider's size (half extents times scale)
   with the entity's `world_bounds`: they should enclose the same volume.
3. **Reproduce slowly.** `sim_play`, immediately `debug_pause`, then
   `debug_watch` the entities with
   `{"components": ["transform", "rigidbody", "collider"]}`. Advance with
   `debug_step` (`{"frames": 1}` to `{"frames": 10}`) and watch again after
   each step to find the exact frame where a position jumps or passes a
   surface. For very fast events `debug_time_scale` `{"scale": 0.25}` and a
   `screenshot` of the paused frame help.
4. **Probe the world.** `debug_raycast` from above the object straight down
   (`"direction": [0, -1, 0]`) shows what is below it: a hit on the expected
   entity, on `static_world` (terrain), or nothing. Cast through a trigger
   with `"include_sensors": true` to check the sensor body exists.
5. **Conclude.** State the cause with the evidence (values per frame, ray
   hits) and propose the smallest fix. Apply it only if asked, then repeat
   steps 3-4 to verify. Finish with `debug_resume`, `debug_time_scale`
   `{"scale": 1}` and `sim_stop`.

## Symptoms and usual causes

- **Falls through the floor:** the floor has no `collider` (a visual
  primitive alone does not collide); its collider is far too small because
  `half_extents` were given in world size and then scaled again, or the
  other way round; terrain was still generating when Play started (check
  `terrain_status`).
- **Jitters, flies apart or explodes on the first frames:** bodies start
  overlapping each other or the ground. Look for overlapping `world_bounds`
  in edit mode and set objects down with `world_place_on_ground` before
  Play.
- **Floats above the ground:** the collider is larger than the mesh, or
  `offset` pushes it down; the bottom of the shape rests, not the mesh.
- **Passes through thin walls (tunnelling):** a small, fast body moves
  further in one step than the wall is thick. Thicken the static collider
  (half extents of 0.25 m or more), make the moving body larger or slower,
  and check `debug_stats` for hitches.
- **Does not fall or move:** `motion` is `static` or `kinematic`, the
  `rigidbody` is missing, or the simulation is paused or slowed
  (`debug_stats` `sim`).
- **Snaps back after you move it:** a dynamic body's transform belongs to
  physics while playing; `scene_update` on it is overwritten next frame.
  Move it in edit mode instead.
- **A collider edit has no effect:** a running body keeps the shape it was
  created with. Stop, edit, and play again.
- **Character stuck or sinking:** its capsule (`radius`, `half_height`)
  overlaps geometry at the start position, or the ground below has no
  collider; raycast down from it.
- **Trigger never fires:** the trigger's collider needs `sensor: true`, and
  the entering object needs a collider with a dynamic or kinematic
  `rigidbody`, or a `character`.

## Pitfalls

- `debug_raycast` and the `physics` count in `debug_stats` need play mode.
- `world_place_on_ground` on a dynamic body during play is undone by the
  simulation; place it in edit mode.
- Changes made during play vanish with `sim_stop`: re-apply the fix in edit
  mode once it is confirmed.
- `debug_step` uses a fixed `dt` (default 1/60 s) regardless of the time
  scale; real frames can be longer, so a glitch that needs a hitch may not
  reproduce under stepping.
