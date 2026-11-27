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
| `engine.info` | — | `{engine, frame, entities, playing, editor}` |

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
  `null` when the engine has no cloud control (no editor layer).
  `sun.color` accepts linear `[r,g,b]` (may exceed 1) or `"#rrggbb"`.
- **Camera angles**: `yaw_deg` is the heading in the XZ plane from +X toward
  +Z (forward = `(cos p cos y, sin p, cos p sin y)`), `pitch_deg` is positive
  looking up, clamped to ±89. `camera.set` takes `look_at` *or*
  yaw/pitch. `camera.frame` keeps the heading, looks down at least 20° and
  backs off until the bounding sphere of what the entity (and its children)
  draws fits the vertical field of view.
- **`render.screenshot`** captures the scene view (the editor viewport
  without UI, else the window), copied on the GPU into a per-frame readback
  buffer that is read after that frame's fence — no stall — then downscaled
  (area filter in linear light) and PNG-encoded off the main thread. At most
  8 may be in flight.
- **Paths** (`scene.save/load`, `asset.spawn_model`) must be relative to the
  project root; absolute paths, `..` escapes and symlinks leading out are
  refused. `scene.save` writes only `.json` and refuses to overwrite a file
  that is not a scene file; save and load refuse while playing.
- **`asset.list_models`** lists `.gltf`/`.glb` under `assets/`.
  **`asset.spawn_model`**: a model with several root nodes is grouped under a
  new entity (named `name`, else the file stem) when `name`, `parent` or
  `components` are given, so they apply to the model as a whole.
- **`sim.play` / `sim.stop`** toggle the editor's play mode (the transition
  runs later that frame); `unavailable` without the editor or in the physics
  / character demo modes, which own their physics world.

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
`"#rrggbb"` or linear `[r,g,b,a]`, `metallic`, `roughness`, `emissive`),
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
`point_light`, `dir_light`, `camera`, `collider`, `rigidbody`, `character`,
`nav_agent`, `behavior`, `patrol`, `terrain`, `net_replicated`,
`audio_source` (`clip`, `bus`: master/music/sfx/ui/ambience, `volume`,
`pitch`, `loop`, `play_on_start`, `spatial`, `min_distance`, `max_distance`,
`rolloff`, `doppler`, plus write-only `command`: play/stop/pause/resume),
`audio_listener` (`active`), `animator` (edit only), `mesh` (read only). Updates merge: omitted fields keep their
values. Unknown fields are errors (they name the known fields).

| Method | Params | Result |
|---|---|---|
| `scene.components` | — | `[{name, summary, addable, writable}]` |
| `scene.list` | `{name_contains?, component?, limit?=100, offset?=0}` | `{total, entities:[{id, name, parent, components:[names], world_position}]}` |
| `scene.get` | `{entity}` | `{id, name, parent, children:[ids], components:{...}}` |
| `scene.create` | `{name?, parent?, components?:{...}}` | `{id, name}` |
| `scene.update` | `{entity, name?, components?:{...}}` | `{id}` |
| `scene.remove_components` | `{entity, components:[names]}` | `{removed:[names]}` |
| `scene.destroy` | `{entity}` (recursive) | `{destroyed: n}` |
| `scene.set_parent` | `{entity, parent, keep_world?=true}` | `{id}` |
| `scene.clear` | `{keep_lights?=true}` | `{destroyed: n}` |
| `scene.batch` | `{commands:[{method, params}], stop_on_error?=true}` | `{results:[{ok:true, result} \| {ok:false, error:{code,message}}]}` — a string `"@N"` in an `entity`/`parent` field means the `id` returned by command N of this batch. Deferred methods cannot be batched. |
| `environment.get` | — | `{sun:{azimuth_deg, elevation_deg, intensity, color}, clouds:{enabled, coverage, density, bottom_km, thickness_km, size_km, detail, wind_speed, wind_dir_deg}}` |
| `environment.set` | same shape, every field optional | `environment.get` result |
| `world.ground_height` | `{points:[[x,z],...]}` (≤ 1024) | `{heights:[y \| null]}` — null where no terrain covers the point |
| `camera.get` | — | `{position, yaw_deg, pitch_deg, fov_deg}` |
| `camera.set` | `{position?, look_at?, yaw_deg?, pitch_deg?, fov_deg?}` | `camera.get` result |
| `camera.frame` | `{entity?}` (whole scene when omitted) | `camera.get` result |
| `render.screenshot` *(deferred)* | `{max_size?=1024}` | `{mime:"image/png", width, height, data:<base64>}` |
| `asset.list_models` | — | `{models:[project-relative paths of .gltf/.glb]}` |
| `asset.spawn_model` | `{path, name?, parent?, components?}` | `{roots:[ids]}` |
| `sim.play` / `sim.stop` | — | `{playing}` |
| `scene.save` / `scene.load` | `{path}` (project-relative; `..` escapes refused) | `{path}` |
| `nav.bake` | — | `{polygons}` |

Sun convention (same as the editor's Renderer panel): the light's travel
direction `d` gives `azimuth_deg = atan2(d.z, d.x)` and `elevation_deg =
asin(-d.y)`.

Methods that need something only the full application has (camera,
screenshot, clouds, glTF spawn, play/stop, save/load, nav bake) return
`unavailable` when the engine runs without it (e.g. in tests).

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
