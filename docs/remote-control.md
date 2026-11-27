# Remote control & MCP

AI assistants drive quat-3d through two pieces:

```
AI client (Claude Code, Claude Desktop, ...)
   │  Model Context Protocol over stdio
   ▼
quat-mcp  (Rust, tools/quat-mcp — planned, ROADMAP A4)
   │  JSON-RPC 2.0 over loopback TCP
   ▼
engine_remote inside the running engine (src/remote/)
```

The engine side exists today: the transport (ROADMAP A1) and the
world-building commands (A2, A3). The Rust MCP server (A4) is next. This page documents the engine
protocol so either side can be built and tested independently.

## Starting the endpoint

```sh
QUAT_REMOTE=7878 ./build/release/game                          # or QUAT_REMOTE=1 for port 7878
QUAT_REMOTE=7878 QUAT_REMOTE_TOKEN=secret ./build/release/game  # require a token
```

The endpoint listens on `127.0.0.1` only. Requests are processed once per
frame on the engine's main thread, before the scene updates, so a command sees
and changes the same state the editor does.

## Wire format

One JSON-RPC 2.0 message per line (`\n`-terminated, compact JSON). Requests
must be objects with `"jsonrpc": "2.0"`, a string `method`, optional object
`params`, and an `id` (string or integer). A request without `id` is a
notification: it runs, and no reply is sent. Batch arrays are not supported —
use the engine's own batch command (A2) instead.

```json
→ {"jsonrpc":"2.0","id":1,"method":"session.hello","params":{"token":"secret"}}
← {"jsonrpc":"2.0","id":1,"result":{"protocol":1,"server":"quat-3d","methods":4}}
→ {"jsonrpc":"2.0","id":2,"method":"engine.info"}
← {"jsonrpc":"2.0","id":2,"result":{"engine":"quat-3d","frame":812,"entities":14,"playing":false,"editor":true}}
```

Try it by hand: `nc 127.0.0.1 7878`, then type a request line.

## Authentication

With `QUAT_REMOTE_TOKEN` set, the first call on a connection must be
`session.hello` with the matching `token`; any other first call, or a wrong
token, gets an `unauthorized` error and the connection is closed. Without a
token, `session.hello` is optional (it still reports the protocol version).

## Methods (implemented)

Everything in the v1 contract below is implemented (`src/remote/world/`,
wired into the game by `src/app/remote_bridge.cpp`), plus:

| Method | Params | Result |
|---|---|---|
| `session.hello` | `{token?}` | `{protocol, server, methods}` |
| `rpc.methods` | — | `[{name, summary, deferred}]` |
| `engine.info` | — | `{engine, frame, entities, playing, paused, editor}` |

`rpc.methods` lists every registered method with a one-line summary.
Details the contract leaves open, as implemented:

- **Entity ids** are entt identifiers including the version bits, so an id
  of a destroyed entity never refers to a later one (`not_found`).
- **Atomic edits.** `scene.create` / `scene.update` validate every component
  on a scratch copy first: any bad field fails the whole call and changes
  nothing. Edits update world transforms immediately (a following
  `scene.get` / `camera.frame` in the same frame sees them).
- **`scene.batch`**: `"@N"` is 0-based (the index in `commands`) and resolves
  to that command's `id` (or the single entry of `asset.spawn_model`'s
  `roots`); it is substituted in the top-level `entity` / `parent` params
  only. The batch's shape is validated before anything runs. `results` always
  has one entry per command; with `stop_on_error` the commands after the
  first failure are reported as `{ok:false, error:{code:-32004, message:"not
  run: ..."}}`. Batches cannot nest; edits are not rolled back.
- **`scene.clear`** with `keep_lights` keeps entities carrying `dir_light` or
  `point_light`, detached to the root at their world placement.
- **`environment.get`**: `sun` is `null` when the scene has no directional
  light (`environment.set {sun}` then creates one named `sun`); `clouds` is
  `null` when the engine has no renderer settings (tests, headless tools). The
  clouds are the `clouds` group of the render settings (same fields, ranges
  and errors as `render.settings.*`).
  `sun.color` accepts linear `[r,g,b]` (may exceed 1) or `"#rrggbb"`.
- **Camera angles**: `yaw_deg` is the heading in the XZ plane from +X toward
  +Z (forward = `(cos p cos y, sin p, cos p sin y)`), `pitch_deg` is positive
  looking up, clamped to ±89. `camera.set` takes `look_at` *or*
  yaw/pitch. `camera.frame` keeps the heading, looks down at least 20° and
  backs off until the bounding sphere of what the entity (and its children)
  draws fits the vertical field of view.
- **Clip planes** (issue #37). The view camera's planes are derived from the
  scene every frame (`engine/scene/view_clip.hpp`): the far plane reaches the
  farthest corner of the drawables' bounds (refreshed twice a second) and of
  the terrain — the whole streaming window around the camera, or the single
  tile — plus 10%, within [500 m, 200 km]; the near plane is far / 50000
  within [0.1, 2] m, which bounds the far/near ratio of the standard-Z D32F
  depth buffer. `camera.set` `near` (0.001..10000) / `far` (1..1000000)
  override either plane; `null` returns it to the derived value; `near_auto` /
  `far_auto` report which are derived. Builds without the editor apply them
  to the camera entity the fly camera drives.
- **Cuts.** `camera.set` `cut: true` marks the move as a teleport: the frame
  restarts temporal history (TAA) instead of blending across the jump;
  `false` is continuous motion (a fly-through of many small calls). Without
  `cut`, a move is a cut when the camera jumps more than 10% of the far plane,
  turns more than 30° or changes its field of view by more than 30°.
  `camera.frame` is always a cut. The flag reaches the frame loop as
  `FrameInputs::camera_cut`, which invalidates the TAA history.
- **Camera lens** (R20): the view's physical lens — `aperture_f` [0.7, 64],
  `shutter_s` [1/16000, 30] s, `iso` [25, 409600], `sensor_height_mm`
  [1, 100], `focal_length_mm` [0, 2000] (0 = `fov_deg` applies; otherwise it
  sets the field of view from the sensor) and `focus_distance_m`
  [0.05, 100000]. It drives depth of field (`dof`), the motion-blur shutter
  and, with `exposure.physical`, the exposure. In the editor it belongs to
  the fly camera (Renderer panel, "View camera lens"); in play through a
  scene camera, and in non-editor builds, the active `camera` component's
  lens fields are used.
- **`render.screenshot`** captures the scene view (the editor viewport
  without UI, else the window), copied on the GPU into a per-frame readback
  buffer that is read after that frame's fence — no stall — then downscaled
  (area filter in linear light) and PNG-encoded off the main thread. At most
  8 may be in flight.
- **Screenshots from an explicit camera** (issue #41). `render.screenshot
  {camera:{...}}` takes the `camera.set` placement fields (`position`,
  `look_at` | `yaw_deg`/`pitch_deg`, `fov_deg`, `near`, `far`; omitted ones
  keep the viewport camera's values) or `frame` (an entity to fit in view, as
  `camera.frame` does). The application renders the next frame from that
  camera (`RemoteBridge::screenshot_view()`), captures exactly that frame and
  goes back to the viewport camera: the viewport camera's state is never
  written, so `camera.get` is unchanged. The renderer has one view, so this
  is a one-frame camera swap rather than a second view: the viewport shows
  the shot for one frame, TAA history restarts on the shot frame and the
  frame after it (both are cuts), auto-exposure keeps adapting to the
  viewport, and terrain keeps streaming around the viewport camera (a shot
  far from it may show missing tiles). Shots with different cameras take a
  frame each; the oldest goes first.
- **Paths** (`scene.save/load`, `asset.spawn_model`) must be relative to the
  project root; absolute paths, `..` escapes and symlinks leading out are
  refused. `scene.save` writes only `.json` and refuses to overwrite a file
  that is not a scene file; save and load refuse while playing.
- **`asset.list_models`** lists `.gltf`/`.glb` under `assets/`.
  **`asset.spawn_model`**: a model with several root nodes is grouped under a
  new entity (named `name`, else the file stem) when `name`, `parent` or
  `components` are given, so they apply to the model as a whole.
- **`sim.play` / `sim.stop`** toggle play mode (the transition runs later
  that frame), in the editor and in the non-editor game alike (which starts
  playing for a `QUAT_SCENE` / `QUAT_PLAY` run); `unavailable` in the physics
  / character demo modes, which own their physics world. `scene.save` /
  `scene.load` are refused (`failed`) while playing.
- **Terrain queries** (issue #39) use the terrain the runtime drives: the
  first entity with a `terrain` component. The runtime samples its tiles
  relative to the terrain's anchor; `world.ground_height` places the anchor
  from the entity's current world transform, so a command later in a
  `scene.batch` sees an earlier move of the terrain (or of a parent). A shape
  edit not yet picked up by the runtime (`regenerate` pending) answers null
  heights with `generating: true` rather than stale ones. Without a terrain
  entity it is `not_found`. **`terrain.status`** tells a client when it can
  place objects or take screenshots: `generating` is true while a tile is
  being generated or uploaded, or the streaming window (`tiles_wanted` =
  (2·`stream_radius`+1)²) is not full; `bounds` covers the loaded tiles.
- **Bounds** (issue #40). `world_bounds` (`{min, max}` or null) is the
  world-space AABB around what the entity and its descendants draw: mesh
  bounds (skinned meshes through their armature), primitives (their unit
  shape before the mesh resolves), every instance of an instanced mesh, and
  the loaded tiles for the terrain entity — each box transformed exactly
  (`engine/scene/bounds.hpp`) through world matrices composed from the local
  transforms, so batch edits count. `local_bounds` (scene.get) is the same
  box in the entity's own frame, terrain excluded: a model's size and where
  its origin sits. Null while nothing draws (lights, empties, a model whose
  mesh is still loading).
- **`world.place_on_ground`** moves the entity (with its subtree) to `at`
  if given, then — with `align_to_normal` — rotates it about its origin so
  its up axis turns toward the surface normal under its bounds' centre (at
  most `max_tilt_deg`; idempotent, the heading is kept), then translates it
  vertically: upright, the bounds' bottom rests on the surfaces sampled at a
  3×3 grid over the footprint (the lowest ground sample or the highest object
  sample, whichever is higher); aligned, the bottom centre of `local_bounds`
  rests on the surface under it. `offset` is added along world up. Surfaces:
  the terrain (analytic heights, normal by central differences) and, below
  the top of the entity's bounds, other entities' non-sensor colliders —
  analytically in edit mode (box, sphere, capsule, with world scale baked in
  as the physics system builds them), or the running physics world's bodies
  and characters in play mode (ownerless bodies such as streamed terrain
  count as ground). The entity's own colliders are skipped. Nothing below is
  `failed`; placing the terrain (or an ancestor of it) is `invalid_params`.
  A failure restores the entity's transform (`at` and the tilt included).
  While a single terrain tile regenerates its old heights are not used
  (`world.ground_height` answers null, placement on it fails). Known gaps
  while a physics world runs: colliders created earlier in the same batch
  are not bodies yet (only the physics world is consulted), and a placed
  dynamic body is pulled back by the simulation.

## Methods (v1 contract, ROADMAP A2/A3)

This is the agreed contract between the engine (`src/remote/`) and the MCP
server (`tools/quat-mcp`). Change it here first if either side needs a
change.

**Entity references.** Wherever a parameter is called `entity` or `parent`
it accepts the entity's integer `id` (as returned by the engine) or a string
name, which must match exactly one entity (otherwise `not_found` or
`invalid_params` listing the ambiguity). `parent: null` means "root".

**Components** are JSON objects keyed by the component names of the shared
codec (`engine/scene/component_json.hpp`): `transform` (`position`,
`rotation` in Euler degrees YXZ, `scale` number or `[x,y,z]`, `look_at`),
`primitive` (`shape`: cube/sphere/plane/cylinder/cone/capsule, `color`
`"#rrggbb"` or linear `[r,g,b,a]`, `metallic`, `roughness`, `emissive`,
`shader`: a material shader name or `"standard"`, `shader_params`: up to 8
numbers — see [Material shaders](#material-shaders-roadmap-a6)),
`instanced_mesh` (many copies of one model in one entity, drawn instanced:
`gltf` path relative to the project root + optional `node`, or — with `gltf`
empty — the `primitive` fields `shape` / `color` / `metallic` / `roughness` /
`emissive`; `cast_shadows`; `instances`: `[{position, rotation (Euler degrees
YXZ), scale (number or [x,y,z])}, ...]` or bare `[x,y,z]` positions, at most
100000, entity-local; read-only `instance_count`. `instances` is omitted —
and cannot be written — while a `scatter` generates them),
`scatter` (seeded placement of the instanced mesh on the terrain around the
entity: `seed`, `area` rect/circle, `size` [x,z] m, `radius` m, `density`
per m², `min_spacing` m, `pattern` poisson/jittered_grid, `scale` [min,max],
`random_yaw`, `align_to_normal` 0..1, `base_offset` (× scale, 0.5 seats a
unit primitive), `slope` [min,max] degrees, `height` [min,max] m,
`max_instances`; regenerates when these, the entity position or the terrain
change; removing it keeps the placements),
`material` (per-entity overrides of the PBR factors the entity's material is
drawn with — see "Look tuning" below; `null` drops one override),
`grass` (GPU grass around the camera, one field per scene — the first
`grass` entity's: `seed`, `density` blades per m² (0..1000), `radius` m
(0..500), `blade_height` / `blade_width` [min,max] m, `base_color` /
`tip_color` `"#rrggbb"` or linear `[r,g,b]`, `wind_strength` 0..4,
`wind_direction` [x,z] (normalised), `wind_frequency` Hz 0..10, `slope`
[min,max] degrees, `height` [min,max] ground height m, `terrain_only` (false:
also flat ground at `ground_y` where no terrain is loaded), `lod_start`
0..1 × radius where blades start thinning, `far_density` 0..1 kept at the
radius),
`point_light`, `dir_light`, `camera`, `collider`, `rigidbody`, `character`,
`nav_agent`, `behavior`, `patrol`, `terrain` (only an edit that changes the
generated heights — `seed`, `resolution`, `tile_size`, `height`, `octaves`,
`warp`, `erosion`, `droplets`, `erosion_steps` — regenerates it; `snowline` is
shading, `stream_radius` resizes the streaming window keeping loaded tiles,
`gpu` picks the generator of the next tile), `net_replicated`,
`audio_source` (`clip`, `bus`: master/music/sfx/ui/ambience, `volume`,
`pitch`, `loop`, `play_on_start`, `spatial`, `min_distance`, `max_distance`,
`rolloff`, `doppler`, plus write-only `command`: play/stop/pause/resume),
`audio_listener` (`active`), `script` (`path` relative to `assets/scripts/`,
`enabled`, `properties`: `{name: number | bool | string | [x,y,z]}`, merged
per name, `null` removes one; see [scripting.md](scripting.md)), `animator`
(edit only: `clip` switches by name among read-only `clips`, `blend`
inertialize/crossfade, `blend_time`, `time`, `speed`, `looping`), `foot_ik`
(`enabled`, `pelvis` / `left_foot` / `right_foot` joint names, empty = auto,
`ray_up`, `ray_down`, `max_adjust`, `max_foot_angle`, `smoothing`), `mesh`
(read only). Updates merge: omitted fields keep their
values. Unknown fields are errors (they name the known fields).

| Method | Params | Result |
|---|---|---|
| `scene.components` | — | `[{name, summary, addable, writable}]` |
| `scene.list` | `{name_contains?, component?, limit?=100, offset?=0}` | `{total, entities:[{id, name, parent, components:[names], world_position, world_bounds}]}` |
| `scene.get` | `{entity}` | `{id, name, parent, children:[ids], world_bounds, local_bounds, components:{...}}` |
| `scene.create` | `{name?, parent?, components?:{...}}` | `{id, name}` |
| `scene.update` | `{entity, name?, components?:{...}}` | `{id}` |
| `scene.remove_components` | `{entity, components:[names]}` | `{removed:[names]}` |
| `scene.destroy` | `{entity}` (recursive) | `{destroyed: n}` |
| `scene.set_parent` | `{entity, parent, keep_world?=true}` | `{id}` |
| `scene.clear` | `{keep_lights?=true}` | `{destroyed: n}` |
| `scene.batch` | `{commands:[{method, params}], stop_on_error?=true}` | `{results:[{ok:true, result} \| {ok:false, error:{code,message}}]}` — a string `"@N"` in an `entity`/`parent` field means the `id` returned by command N of this batch. Deferred methods cannot be batched. |
| `environment.get` | — | `{sun:{azimuth_deg, elevation_deg, intensity, color}, clouds:{enabled, coverage, density, bottom_km, thickness_km, size_km, detail, wind_speed, wind_dir_deg}}` |
| `environment.set` | same shape, every field optional | `environment.get` result |
| `world.ground_height` | `{points:[[x,z],...]}` (≤ 1024) | `{heights:[y \| null], generating}` — null where no tile is loaded; `not_found` without terrain |
| `terrain.status` | — | `{present, entity, streaming, generating, tiles_loaded, tiles_wanted, anchor, tile_size, bounds:{min,max} \| null}` (`{present:false}` without terrain) |
| `world.place_on_ground` | `{entity, at?:[x,z], align_to_normal?=false, max_tilt_deg?=30, offset?=0}` | `{id, position, world_bounds, moved_y, tilt_deg, surface:{kind, entity, y, normal} \| null}` |
| `camera.get` | — | `{position, yaw_deg, pitch_deg, fov_deg, near, far, near_auto, far_auto, lens: {aperture_f, shutter_s, iso, sensor_height_mm, focal_length_mm, focus_distance_m}}` |
| `camera.set` | `{position?, look_at?, yaw_deg?, pitch_deg?, fov_deg?, near?, far?, cut?, lens?}` (`lens` merges: omitted fields keep their values) | `camera.get` result |
| `camera.frame` | `{entity?}` (whole scene when omitted) | `camera.get` result |
| `render.screenshot` *(deferred)* | `{max_size?=1024, camera?:{position?, look_at? \| yaw_deg?/pitch_deg?, frame?, fov_deg?, near?, far?}}` | `{mime:"image/png", width, height, data:<base64>}` |
| `asset.list_models` | — | `{models:[project-relative paths of .gltf/.glb]}` |
| `asset.spawn_model` | `{path, name?, parent?, components?}` | `{roots:[ids]}` |
| `sim.play` / `sim.stop` | — | `{playing}` |
| `scene.save` / `scene.load` | `{path}` (project-relative; `..` escapes refused) | `{path}` |
| `nav.bake` | — | `{polygons}` |

Sun convention (same as the editor's Renderer panel): the light's travel
direction `d` gives `azimuth_deg = atan2(d.z, d.x)` and `elevation_deg =
asin(-d.y)`.

Methods that need something only the full application has (camera,
screenshot, clouds, glTF spawn, play/stop, save/load, nav bake, and the
debugging services: log, clock, physics world, navmesh) return
`unavailable` when the engine runs without it (e.g. in tests).

## Debugging methods (ROADMAP A5)

Methods that let a model diagnose a running game. Contract first: the
engine (`src/remote/world/debug_*.cpp`, services wired by
`src/app/remote_bridge.cpp`) and quat-mcp's `debug_*` tools follow this table.

| Method | Params | Result |
|---|---|---|
| `debug.log` | `{since_seq?, level?, category?, contains?, limit?=100}` (limit 1..1000) | `{lines:[{seq, t, level, category, message}], cursor, more, missed, oldest_seq, capacity}` |
| `debug.stats` | — | `{frame, frame_time_ms:{last, avg, worst, samples}, fps, draws, memory:{resident_bytes}, entities, physics:{bodies, active_bodies, characters}, nav:{agents, moving, failed, navmesh_polygons}, sim:<clock>, log:{last_seq, warnings, errors, last_error_seq}}` |
| `debug.pause` / `debug.resume` | — | `<clock>` |
| `debug.step` *(deferred)* | `{frames?=1, dt?=1/60}` (frames 1..10000, dt in [0.0001, 1/15] s) | `<clock>` once the steps have run |
| `debug.time_scale` | `{scale}` in [0.01, 10] | `<clock>` |
| `debug.watch` | `{entity}` or `{entities:[...]}` (≤ 64), `components?:[names]` | `{entities:[{id, name, world_position, components:{...}, missing?:[names], nav_agent_state?}], frame?, sim_time_s?}` |
| `debug.raycast` | `{origin, direction, max_distance?=1000, include_sensors?=false}` | `{hit:false}` or `{hit:true, entity, name, kind, point, normal, distance, sensor}` |
| `debug.nav_path` | `{from, to}` | `{points:[[x,y,z]...], partial, length}` |

`<clock>` is `{paused, time_scale, pending_steps, steps_run, sim_frames,
sim_time_s}`. `engine.info` also reports `paused`.

- **Log.** The engine keeps the most recent 4096 log lines in a ring
  (`engine/core/log.hpp`): lines written through `core::log()` (category as
  given; Vulkan validation messages arrive as category `vulkan`) and — when
  the endpoint is enabled — everything the process writes to stderr, tee'd
  through a pipe (`QUAT_LOG_CAPTURE=0` disables the capture). Captured lines
  are parsed for leading tags: `[warn] x` is level `warn`, category `stderr`;
  `[nav] x` is level `info`, category `nav`; `[vulkan][ERROR] x` is `error` /
  `vulkan`. `level` is the minimum level (`trace` < `debug` < `info` <
  `warn` < `error`), `category` an exact match, `contains` a case-sensitive
  substring. Without `since_seq` the call returns the newest `limit`
  matching lines (oldest first); with it, the lines after `since_seq`.
  Either way pass `cursor` back as the next `since_seq` to tail the log;
  `more` means `limit` cut the read short, `missed` counts lines after
  `since_seq` that the ring overwrote before they were read. A `since_seq`
  beyond the newest line (a cursor from before an engine restart) reads from
  the oldest retained line. `t` is seconds since the log started.
- **Stats.** Frame times are real (wall-clock) frame times over the last 240
  frames; `draws` counts the previous frame's scene draws after instancing
  (opaque + shadow batches, InstancedMesh batches included, plus one per
  transparent item); `memory` is the process resident set (Linux; `null`
  elsewhere); `physics` is `null` while
  no physics world runs (edit mode). `nav.moving` counts active agents that
  have not arrived, `nav.failed` agents that gave up on their target. Fields
  the running engine cannot provide are `null`.
- **Simulation clock.** The frame loop feeds the simulation (physics,
  characters, behaviour trees, navigation agents, animation) a clock dt:
  real dt × `time_scale` while running, 0 while paused. The camera, editor,
  audio, networking and rendering keep running in real time, so a paused
  scene can still be inspected, screenshotted and flown around.
  `debug.step` pauses (if running) and queues `frames` single steps, each
  advancing the simulation by exactly `dt` (unscaled) on one rendered frame
  (one loop iteration while the window is minimised);
  the reply comes after the last step ran, so a following `debug.watch`
  sees the result. Further requests queue behind pending steps, each keeping
  its own `dt`; at most 100000 steps, 64 runs of differing `dt` and 64
  waiting requests are accepted (beyond that a request fails with `failed`
  and queues nothing).
  `debug.resume` drops pending steps (their `debug.step`
  replies then complete). Every simulation system advances by the same
  simulated time: real frame time beyond 0.25 s (a hitch) is dropped for
  all of them, a step's `dt` is at most 1/15 s (what one physics update —
  8 fixed 1/120 s substeps — simulates exactly; `debug.step` refuses a
  larger `dt` with `invalid_params` naming `dt` and queues nothing, rather
  than silently clamping it, so a stepped trace never runs with a dt the
  caller did not ask for), and a frame above 1/15 s (a slow
  frame, or any frame at a high time scale) runs the simulation in equal
  slices of at most 1/15 s, so physics
  keeps up with agents, characters and animation at any time scale. The
  clock state survives play/stop: a paused clock stays paused when play
  starts.
- **Watch** reuses the component codec (same shapes as `scene.get`);
  `components` restricts the snapshot (unknown names are `invalid_params`,
  names the entity lacks are listed in `missing`). Entities with a nav agent
  also get `nav_agent_state` — the system-managed state the codec does not
  expose: `active, paused, arrived, failed, commanded, plan_failures,
  path_partial, waypoint, path, retry_s, stuck_s`.
- **Raycast** runs against the physics world being simulated — it exists
  only in play mode (or the physics/character demo modes); otherwise
  `unavailable`. It hits rigid bodies, static colliders, terrain height
  fields and character capsules; sensors only with `include_sensors`.
  `direction` need not be normalized (zero is `invalid_params`). `kind` is
  `body` (owned by `entity`), `character`, or `static_world` (a body no
  entity owns: streamed terrain, demo floors — `entity` is null).
- **Nav path** snaps both ends to the baked navmesh (within ±4 m
  horizontally, ±16 m vertically) and returns the string-pulled path from
  the snapped start; `partial` means the target is unreachable and the path
  ends at the closest reachable point. No baked navmesh is `unavailable`
  (call `nav.bake`); an end off the mesh is `failed`.

## Look tuning (ROADMAP A8)

Every tunable renderer parameter, and the PBR factors an entity is drawn
with, are readable and writable remotely — the same values the editor's
Renderer panel and Material inspector show, because all of them go through
one descriptor table each (`engine/renderer/render_settings.hpp`,
`engine/scene/material_override.hpp`).

| Method | Params | Result |
|---|---|---|
| `render.settings.get` | `{group?}` | `{<group>: {<field>: value}}` — every group, or only `group` |
| `render.settings.set` | `{<group>: {<field>: value}, ...}` | every group's new values |
| `render.settings.reset` | `{group?}` (every group when omitted) | every group's values |
| `render.settings.schema` | — | `{groups: [{name, label, hint, fields: [{name, label, type, min?, max?, enum?, default, unit?, hint}], deprecated?: [{name, note}]}], material: [{name, label, kind, min, max, hint, primitive_field}]}` |
| `material.get` | `{entity}` | `{id, name, source: "primitive"\|"mesh", material: {...}, overridden: [fields], textures: [slots], transparent, shared_by?}` |
| `material.set` | `{entity, material: {<field>: value \| null}}` | `material.get` result |

Settings groups (the renderer implements exactly these):

| Group | Fields |
|---|---|
| `exposure` | `auto` (histogram auto exposure; off = fixed exposure 2^EV), `compensation_ev` [-10, 10] EV (applies in every mode), `key` [0.01, 1], `min_exposure` / `max_exposure` [0.001, 64] (auto multiplier clamps), `adaptation_s` [0.05, 10] s, `physical` (exposure from the active camera's aperture / shutter / ISO: 1 / (1.2 · 2^EV100); overrides `auto`) |
| `bloom` | `enabled`, `threshold` [0, 8], `knee` [0, 1], `intensity` [0, 1], `radius` [0.2, 2] |
| `taa` | `enabled` (off: no jitter, no history — the frame passes through — and native resolution), `feedback` [0, 0.98] (history weight; the resolve caps a pixel's accumulated sample weight at 1 / (1 − feedback)) |
| `upscaler` | `mode` `"native_aa"` \| `"quality"` \| `"balanced"` \| `"performance"` (render scale 1, 1/1.5, 1/1.7, 1/2 per axis; needs TAA on), `render_scale` [0, 1] (overrides the mode when > 0, min 0.25), `sharpness` [0, 1], `backend` `"taau"` \| `"fsr2"` (FSR 2 is not built yet: falls back to `taau`) |
| `shadows` | cascaded sun shadows: `enabled` (off: no shadow passes), `max_distance` [5, 5000] m (where shadows end), `cascades` integer [1, 4], `split_lambda` [0, 1] (0 linear, 1 logarithmic splits), `fit_to_depth` (fit the cascades to the frame's depth range on the GPU), `blend_band` [0, 0.5], `pcf_radius` [0, 8] texels, `sun_size_deg` [0, 10] (PCSS penumbra; 0 = fixed filter), `normal_bias_texels` / `depth_bias_texels` [0, 8], `caster_distance` [1, 10000] m, `debug_cascades`. Deprecated (accepted, ignored): `extent_m`, `depth_bias`, `slope_bias` |
| `ao` | GTAO: `enabled`, `radius` [0.05, 10] m, `strength` [0.5, 5], `falloff_range` [0, 1], `sample_distribution_power` [1, 4], `thin_occluder_compensation` [0, 0.7], `depth_mip_offset` [0, 8], `slices` integer [1, 9], `steps_per_slice` integer [1, 8], `denoise_passes` integer [0, 3], `denoise_blur_beta` [0.1, 10], `bent_normals`, `specular_occlusion`, `multi_bounce` |
| `contact_shadows` | `enabled`, `length` [0.01, 4] m, `steps` integer [2, 64], `thickness` [0.01, 4] m, `intensity` [0, 1] |
| `ibl` | `intensity` [0, 4] — scales image-based ambient light and reflections |
| `clouds` | as `environment.*`: `enabled`, `coverage` [0, 1], `density` [0, 128] /km, `bottom_km` [0.2, 15], `thickness_km` [0.2, 10], `size_km` [1, 80], `detail` [0, 1], `wind_speed` [0, 100] m/s, `wind_dir_deg` [-180, 180] |
| `dof` | depth of field from the active camera's lens (`camera` component: `aperture_f`, `focal_length_mm`, `focus_distance_m`, `sensor_height_mm`): `enabled` (default off), `max_radius_px` [4, 64], `rings` integer [2, 6] |
| `motion_blur` | `enabled`, `max_radius_px` [8, 64] (also the tile size), `samples` integer [4, 32], `scale` [0, 2] × the camera's shutter fraction (`shutter_s` / frame time, at most 1) |
| `tonemap` | `operator` `"aces"` \| `"agx"` \| `"pbr_neutral"` |
| `grading` | `temperature_k` [1500, 15000] K and `tint` [-1, 1] (white balance; 6500 / 0 neutral), `contrast` [0, 2], `saturation` [0, 2], `lift_r/g/b` [-0.5, 0.5], `gamma_r/g/b` [0.2, 5], `gain_r/g/b` [0, 4] (in ACEScct), `lut` (project-relative `.cube` path, `""` = none; checked on set — a missing or malformed file is rejected with the parser's error), `lut_contribution` [0, 1] |
| `film` | `vignette_natural` [0, 1] (cos⁴ falloff of the camera's field of view), `vignette_intensity` [0, 1], `vignette_smoothness` [0.05, 1], `grain_intensity` [0, 1], `grain_size_px` [0.5, 4], `chromatic_aberration` [0, 1] |

The renderer has no fog or debug views, so neither is listed. The sun is
scene data: `environment.*`. Field types in the schema: `boolean`, `number`,
`integer`, `string` with `enum` (a choice) or with `format: "path"` and an
`extension` (a project file).

- **Set** merges: absent groups and fields (and fields given as `null`) keep
  their values. Every value is type- and range-checked and the call is all or
  nothing; errors name the field and its range, e.g.
  `exposure.compensation_ev: expected a number in [-10, 10] (EV)`, and unknown
  groups / fields list the known ones. Changes apply from the next frame.
  Without the renderer's settings (tests, headless tools) get / set / reset
  are `unavailable`; the schema is static and always available.
- **Materials.** Fields: `base_color` (linear `[r, g, b(, a)]` in [0, 1] or
  `"#rrggbb"` / `"#rrggbbaa"` sRGB), `emissive` (linear `[r, g, b]` in
  [0, 1000] or `"#rrggbb"`), `metallic`, `roughness`, `occlusion_strength`,
  `alpha_cutoff`, `transmission`, `clearcoat`, `clearcoat_roughness` [0, 1],
  `normal_scale` [0, 4], `ior` [1, 3], `attenuation_color` [0, 1] RGB,
  `attenuation_distance` and `thickness` [0, 10000]. On a primitive,
  `base_color`, `metallic`, `roughness` and `emissive` write the `primitive`
  component itself (and drop a `material` override of that field); every
  other field — and every field of a glTF / code-created material — is stored
  in the entity's `material` component, an override applied on top of the
  shared material asset, which itself never changes (`shared_by` counts the
  entities drawing it). `null` reverts one override. A `transmission` above 0
  routes the material to the forward glass pass. The derived material is
  each entity's own copy of its base material with the overrides applied, drawn
  from the next frame; a replaced copy (every edit makes one) is destroyed
  once the frames in flight that may read it have completed, so tuning never
  accumulates materials. Instanced meshes keep their material in
  `instanced_mesh`
  (`invalid_params`); an entity without a primitive or mesh has no material
  (`invalid_params`).
- **Scene files** carry the settings in an optional, versioned
  `"render_settings"` block (`{"version": 1, "<group>": {...}}`) and material
  overrides as the `material` component. A file without the block loads with
  the default settings. Files decode leniently: unknown groups / fields are
  ignored and out-of-range numbers clamped (in the block and in `material`
  overrides). A block whose `version` is newer than the build reads, or a
  malformed one, fails the load and leaves the scene and settings untouched.

## Behaviour-tree authoring (ROADMAP A7)

Methods that build, inspect and edit an entity's behaviour tree — the same
graph the editor's Behaviour panel (G2) shows. Contract first: the engine
(`src/remote/world/behavior_commands.cpp`, on the graph model in
`engine/scene/bt_graph.hpp`) and quat-mcp's `bt_*` tools follow this table.

| Method | Params | Result |
|---|---|---|
| `bt.leaves` | — | `{kinds:[{name, min_children, max_children, help}], leaves:[{name, help, params:[{name, type, default, min?, max?, optional, help}]}]}` |
| `bt.get` | `{entity}` | `<tree view>` |
| `bt.set` | `{entity, tree, enabled?}` | `<tree view>` |
| `bt.add_node` | `{entity, parent, index?, kind, leaf?, params?, children?}` | `<tree view>` + `node` (the new id) |
| `bt.remove_node` | `{entity, node, keep_children?=false}` | `<tree view>` |
| `bt.move_node` | `{entity, node, parent, index?}` | `<tree view>` |
| `bt.set_params` | `{entity, node, params:{name: number \| null}}` | `<tree view>` |
| `bt.status` | `{entity}` | `{entity, playing, enabled, compiled, tick, root_status, nodes:[{id, path, status}]}` |

`<tree view>` is `{entity, enabled, compiled, parse_failed, root, nodes:[{id,
kind, leaf?, params?, children:[ids], path, position:[x, y]}], issues:[{node,
path?, message}], tree}`: `nodes` in depth-first pre-order (the order the
runtime compiles them in), `root` the root's id (null for an empty tree),
`tree` the tree JSON without the editor's layout field, `issues` what keeps
the stored tree from running (empty for a valid tree).

- **Trees** are the runtime's JSON (`engine/scene/behavior.hpp`):
  `{"type": <kind>, "children": [...]}` for composites, `{"type": "leaf",
  "name": <leaf>, <param>: <number>...}` for leaves. `bt.leaves` lists the
  kinds and every leaf with its parameters (defaults, ranges, which may be
  omitted) from the runtime's reflection table. `tree` may be an object or
  its JSON text. Parameters a leaf node omits take the leaf's default.
- **Node ids** are stable: they are stored with the layout in the tree's
  root `"editor"` field (the Behaviour panel's ids and positions), kept by
  every edit, and new nodes get the next unused id. A tree written without
  that field gets ids 1..N in pre-order. `bt.set` with a tree of the same
  shape as the current one (same kinds, leaves and child counts in pre-order,
  e.g. only parameters changed) keeps the current ids and layout. `path` is
  the node's place in the tree JSON (`root.children[1].children[0]`), the
  same spelling runtime parse errors use.
- **Validation.** Every write validates the resulting tree first (and then
  compiles it with the runtime, the final authority) and never stores an
  invalid tree: a failure is `invalid_params` whose message names each
  problem with the node's id and path (`node 4 at root.children[1]:
  wait.seconds: expected a number in [0, 1000000]`), with the same list as
  `data: {issues:[{node, path, message}]}`. Unknown kinds, leaves and
  parameters list the known ones. A successful write sets
  `BehaviorTree::source` and recompiles, exactly like the panel and the
  inspector (the tree restarts from its root); `bt.set` creates the
  `behavior` component when the entity lacks it, the other methods answer
  `not_found` then.
- **Edits.** `parent`/`node` are node ids. `index` is the position among the
  parent's children once the node being placed (and, for `bt.add_node`, any
  adopted children) has been taken out; it defaults to the end. `bt.add_node`
  adds a leaf (`kind: "leaf"`, `leaf`, `params`) or a composite whose
  `children` are tree-JSON node objects (new subtrees) or ids of existing
  nodes (moved under it with their subtrees — e.g. wrapping a node in an
  inverter). `parent: null` makes the new node the root: allowed for an
  empty tree, or when it adopts the current root. `bt.remove_node` removes
  the node with its subtree, or with `keep_children` splices its children
  into its place; the root can only be removed with `keep_children` when it
  has exactly one child. `bt.move_node` re-parents or re-orders a node with
  its subtree (cycles are refused). `bt.set_params` merges leaf parameters:
  a number sets one, `null` removes it (back to the default).
- **Layout.** A tree whose layout is the automatic one stays automatically
  laid out after an edit; otherwise new and moved nodes are placed between
  their siblings below their parent, and the whole tree is re-laid out only
  if sibling order on screen would no longer match child order (the panel
  orders children by x).
- **Status.** `status` per node is what it returned on the tree's last tick:
  `running`, `success`, `failure`, or `idle` (not reached that tick).
  `root_status` is the root's. Trees tick only during Play, so outside Play
  the last tick of the previous session (or nothing) is reported;
  `tick` counts ticks since the tree was compiled. `compiled: false` (not
  ticked since the last change) reports no nodes. Edits work during Play
  too (the tree restarts from its root), but like every edit made while
  playing they are undone when Play stops and the editor restores the scene
  it started from.
## Material shaders (ROADMAP A6)

A model (or a person) can author **material shaders**: Slang modules that
compute a surface — albedo, normal, metallic, roughness, occlusion, emission,
alpha — from the shading point, the entity's material parameters and time.
They are compiled by the running engine (shader hot reload, R2), assigned to
entities through the `primitive` component, and swapped into the running
renderer without a restart.

### How a material shader plugs in

```
assets/shaders/lib/material.slang          IMaterial, MaterialInput, SurfaceSample, StandardMaterial
assets/shaders/lib/gbuffer_common.slang    the GBuffer pass, generic over M : IMaterial
assets/shaders/gbuffer.slang               built-in program: gbuffer_evaluate<StandardMaterial>
assets/shaders/lib/material_pass_gbuffer.slang
                                           driver: entry points + `extern struct SurfaceMaterial : IMaterial`
assets/shaders/materials/<name>.slang      a material: `struct X : IMaterial {...}`
                                           and `export struct SurfaceMaterial : IMaterial = X;`
assets/shaders/templates/material.slang    the starter module (shader.template)
cooked/shaders/materials/<name>.gbuffer.spv (+ .json reflection)
```

- **Interface.** `IMaterial` has one static function,
  `SurfaceSample evaluate(MaterialInput input)`. `MaterialInput` carries `uv`,
  `world_pos`, `normal` (world, unit), `view_dir` (toward the camera), `time`
  (seconds), `params` (the entity's `PbrMaterialParams`, including two free
  `shader_params` vec4s) and `standard` — the built-in glTF PBR result at that
  point, textures applied, so a material can start from it and change only what
  it is about (unused parts are compiled out). The built-in material is
  `StandardMaterial`, which returns `standard` unchanged: the built-in and the
  authored programs run the same pass code.
- **Link-time specialisation, one program per material.** A material is
  compiled with one slangc run per pass variant: the material source plus the
  variant's cooked driver module
  (`slangc <name>.slang cooked/material_pass_gbuffer.slang-module -I lib -I cooked ...`).
  The driver declares `extern struct SurfaceMaterial : IMaterial` and the
  material exports it, so the program is linked and specialised by Slang — no
  source splicing, no `#ifdef`s, and the generated code is what a hand-written
  pass for that material would be. Errors point into the material's own file
  with line and column. Every program has exactly the built-in program's
  pipeline layout (the parameters live in `gbuffer_common.slang`); a material
  cannot declare resources of its own — the registry and the renderer both
  check the reflection against the built-in program and reject a misfit.
- **Trade-off.** This is "one pipeline per material shader": each authored
  material costs one SPIR-V program and one graphics pipeline per pass variant,
  and the GBuffer pass binds one pipeline per material shader in use each frame
  (draws are grouped by program; the material descriptor set and push constants
  are shared). The alternative — one über-pipeline that branches on a material
  id, or dynamic dispatch through an existential `IMaterial` — keeps a single
  pipeline but makes every pixel pay for the union of all materials (registers,
  divergence) and forces a full pass recompile for any edit. Authored materials
  are few and edited often, so per-material programs win: compile ~0.3–1 s per
  edit on a worker thread, pipeline creation once, zero cost to other
  materials.
- **Deferred only (v1).** Material shaders run in the GBuffer pass, so they are
  lit, shadowed, reflected, bloomed and exposed like everything else; emission
  goes to a new GBuffer target the lighting pass adds. Their `alpha` is a
  cutout (below 0.5 discards the pixel); a `primitive` with a material shader
  never routes to the blended forward pass, and shadow casting ignores the
  cutout (a cut-out hole still casts the mesh's shadow). Shadow and forward
  (blended) variants are a follow-up (ROADMAP A6): the variant table
  (`include/engine/rhi/material_shader_names.hpp`) and the compile path already
  handle several drivers per material.
- **Selection.** `primitive.shader` (also on `instanced_mesh`'s primitive
  fields) names the material shader; `"standard"` / `""` is the built-in.
  `primitive.shader_params` is up to 8 numbers filling
  `params.shader_params[0..1]` in order. Entities that name a shader that is
  not (yet) compiled render with the built-in program; the renderer retries
  every 30 frames, so a shader created after the assignment appears on its own.
  `MaterialInput::time` is seconds since the renderer started. A per-entity
  `material` override (A8) draws a derived copy that keeps the shader and its
  `shader_params` (they are not override fields — edit them on the
  `primitive`); the overridden factors reach the shader through
  `MaterialInput::params` and `standard`.
- **Transactional edits.** `shader.create` / `shader.update` write the file,
  have the hot reload service compile it, and reply when the compile finished.
  A failed create deletes the file again; a failed update restores the previous
  text (which recompiles, so the status returns to it). The cooked programs are
  only replaced by a successful compile, so the previous version keeps
  rendering throughout. Operations run one at a time, in request order.

### Methods

| Method | Params | Result |
|---|---|---|
| `shader.list` | — | `{shaders:[{name, builtin, path, state, reloads, errors}], can_compile}` |
| `shader.get` | `{name}` | `{name, builtin, path, state, reloads, errors, source, diagnostics, log}` |
| `shader.template` | — | `{source, save_as}` |
| `shader.create` *(deferred)* | `{name, source}` | `{name, path, ok, errors, diagnostics, log}` (+ `kept_previous:false` when not ok) |
| `shader.update` *(deferred)* | `{name, source}` | same; `kept_previous:true` when the old version was restored |
| `shader.delete` | `{name}` | `{deleted, entities_reset}` |

- **Names** are lowercase identifiers (`[a-z][a-z0-9_]*`, at most 48
  characters): the file stem under `assets/shaders/materials/`, so a name can
  never address a path elsewhere. `standard` and the names of shader library
  modules (`material`, `pbr`, ...) are reserved. `source` is at most 256 KiB.
- **Source confinement.** slangc would read any file a source names, and its
  diagnostics would echo the lines back, so the source is checked before it is
  written: outside comments and string literals it may contain no `#`
  (preprocessor directives), no `__include`, no backslash, and every
  `import` must be `import <name>;` with `<name>` a shader library module.
  Anything else is `invalid_params` naming the line (`source line N: ...`);
  nothing is written.
- **`state`**: `builtin`, `cooked` (as built offline, not recompiled this
  run), `compiling`, `ok`, `error` (the last compile failed; the previous
  version is what runs), or `not_compiled` (no cooked program and no compiler
  in this run).
- **A compile failure is a result, not an RPC error**: `ok:false` with the
  diagnostics. Each diagnostic is `{severity, message, file?, line?, column?,
  code?, source_line?}` — `file` project-relative, `line`/`column` 1-based from
  slangc's caret line, `code` slang's diagnostic id; link errors have no
  location. A misfit layout (the material declared its own resources) is an
  error diagnostic without a location. `log` is slangc's full output with
  project paths shortened. A compile that does not finish in time (slangc is
  killed after the compiler's timeout; the registry waits a little longer from
  when the compile started) is reported the same way, `ok:false` with an error
  diagnostic, and undone like any failed compile.
- **Errors**: `invalid_params` (bad name / source, reserved name),
  `not_found` (`shader.get` / `update` / `delete` of an unknown name),
  `failed` (`shader.create` of an existing name — use `shader.update`; a
  delete while that shader is compiling; file system errors), `unavailable`
  (no registry in this engine, or
  `create` / `update` while shader hot reload is off — run with
  `QUAT_SHADER_HOT_RELOAD=1`, the default in editor builds).
- **`shader.delete`** removes the source and its cooked programs and resets
  every `primitive.shader` (and `instanced_mesh` primitive) that named it to
  the built-in, reporting how many.

Typical loop: `shader.template` → edit → `shader.create` → read `diagnostics`,
fix, `shader.update` → `scene.update {entity, components:{primitive:{shader,
shader_params}}}` → `render.screenshot` → iterate.

## Gameplay scripts (ROADMAP G7)

Lua script files under `assets/scripts/` (the language reference is
[scripting.md](scripting.md)); entities run them through the `script`
component (`scene.create` / `scene.update`). Contract for the engine
(`src/remote/world/script_commands.cpp`) and quat-mcp's `script_*` tools:

| Method | Params | Result |
|---|---|---|
| `script.list` | — | `{scripts:[{path, bytes, entities, status?}], running}` |
| `script.get` | `{path}` | `{path, source, entities, status}` |
| `script.create` | `{path, source}` | `{path, bytes, entities, running?, status?}` |
| `script.update` | `{path, source}` | same as `script.create` |
| `script.delete` | `{path}` | `{path, deleted, entities_still_using}` |
| `script.check` | `{source, path?}` | `{ok, line?, message?}` |

`status` is `{state: "ok" \| "error", message?, instances, failed, reloads}`
(null before the runtime has seen the file); `entities` counts the entities
whose `script` component uses the file; `running` is true while a play
session runs scripts.

- **Paths** are relative to `assets/scripts/`, end in `.lua` and use only
  `[A-Za-z0-9_.-]` segments separated by `/` (no `.` / `..` segments); a
  path whose existing prefix resolves outside the directory (a symlink) is
  refused. Parent directories are created.
- **Sources** are at most 256 KiB of text. `script.create` / `script.update`
  compile the source first: a syntax error is `invalid_params` with the
  message `syntax error at line N: <path>:N: <Lua's message>` and `data:
  {path, line}`, and nothing is written. Writes go through a temporary file
  and a rename.
- `script.create` refuses an existing file (`failed`), `script.update` a
  missing one (`not_found`).
- `script.update` (and `script.create`) hot-reload the file at once: while
  playing, every instance restarts with the new code; a file that fails to
  load at run time (an error in its top-level code) keeps the previous
  version running and its `status` carries the error. Runtime errors from
  callbacks are in `debug.log` (category `script`) and in `status`.
- `script.delete` does not detach the file from entities; running instances
  keep their loaded code until the session ends.

## Asset downloads (quat-mcp only, ROADMAP A9)

The engine never downloads anything. quat-mcp's `asset_search` /
`asset_fetch` talk to the public Poly Haven API themselves
(`tools/quat-mcp/src/assets/`): search filters `/assets?t=models` by keyword
and category and reports per-resolution download sizes from `/files/<id>`;
fetch downloads the glTF and its `include` files into
`<project>/assets/meshes/polyhaven/<id>/` (`--project-root`, default the
current directory). Ids must be Poly Haven slugs and every path from the API
must be a plain relative path (no `..`, absolute, hidden or backslash
components); every URL (and redirect, at most 3) must be https on
`dl.polyhaven.org` or the API's host, or share the configured API's exact
origin; every non-`data:` `buffers[].uri` / `images[].uri` of the glTF
(percent-decoded) must name one of the downloaded files; the declared total is capped (`--asset-max-mb`, default 512)
before anything is downloaded, and each file must match its declared size
and MD5. Files land in a staging directory private to the request (concurrent
fetches of one model never share it) that is moved into place only
when everything verified, next to `LICENSE.md` (CC0, authors, source) and
`polyhaven.json` (provenance). With `quat-cook` on PATH the model is also
cooked to `<id>.glb`. The folder is gitignored. The engine then loads the
files through `asset.spawn_model` / `instanced_mesh` like any other model.

## Errors

| Code | Meaning |
|---|---|
| -32700 | Invalid JSON |
| -32600 | Not a valid request (also: message larger than the limit, 16 MiB) |
| -32601 | Unknown method |
| -32602 | Bad parameters — the message names the field and what was expected |
| -32603 | Internal error (also: a deferred request was dropped) |
| -32001 | Unauthorized (missing or wrong token) |
| -32002 | Unavailable in this build or state |
| -32003 | Referenced entity/asset not found |
| -32004 | The operation itself failed |

Error messages are written for the caller to act on (e.g.
`point_light.radius: expected a number >= 0`), because language models correct
themselves from them.

## Limits

- At most 4 simultaneous clients; extra connections get an error and are closed.
- At most 512 requests are processed per frame; the rest wait for the next frame.
- A client that stops reading is dropped once 256 MiB of replies are queued.

## Engine-side API

Register methods from engine code (main thread only):

```cpp
server.add_method("scene.count", "Number of entities.",
    [&](const engine::remote::Json& params) -> engine::remote::RpcResult {
        return engine::remote::Json(scene.registry().view<Transform>().size());
    });

// Replies later (e.g. after a GPU readback): keep the Responder, call reply().
server.add_deferred_method("render.screenshot", "...",
    [&](const engine::remote::Json& params, engine::remote::Responder r) {
        pending_shots.push_back(std::move(r));
    });
```

Validate parameters with `engine::core::JsonFields` and return
`rpc_fail(rpc_code::invalid_params, message)` on bad input — never call
`nlohmann::json::get` on request data directly (the engine has no exceptions).
