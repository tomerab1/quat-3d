# quat-mcp

A [Model Context Protocol](https://modelcontextprotocol.io) server that lets AI
assistants — Claude Code, Claude Desktop, or any MCP client — build and
inspect worlds in a running quat-3d engine: create and edit entities, set the
sun and clouds, move the camera, take screenshots, spawn glTF models, play the
simulation, save scenes.

```
AI client ──MCP over stdio──▶ quat-mcp ──JSON-RPC 2.0 over loopback TCP──▶ engine (QUAT_REMOTE)
```

quat-mcp is a small, standalone Rust program (built on the official
[`rmcp`](https://crates.io/crates/rmcp) SDK). It is not part of the CMake
build. The engine never speaks MCP itself; it exposes the remote-control
protocol in [`docs/remote-control.md`](../../docs/remote-control.md), and
quat-mcp translates.

## Build

Needs a Rust toolchain (1.88 or newer; install with [rustup](https://rustup.rs)).

```sh
cd tools/quat-mcp
cargo build --release
# binary: tools/quat-mcp/target/release/quat-mcp
```

## Run the engine with the remote-control endpoint

```sh
QUAT_REMOTE=7878 ./build/release/game                            # listen on 127.0.0.1:7878
QUAT_REMOTE=7878 QUAT_REMOTE_TOKEN=s3cret ./build/release/game   # also require a token
```

quat-mcp connects lazily on the first tool call and reconnects automatically
if the engine is restarted, so the engine and the AI client can be started in
any order.

## Configuration

Every option is a flag or an environment variable:

| Flag | Environment | Default | Meaning |
|---|---|---|---|
| `--port` | `QUAT_REMOTE_PORT` | `7878` | Engine port (the engine's `QUAT_REMOTE` value) |
| `--host` | `QUAT_REMOTE_HOST` | `127.0.0.1` | Engine host |
| `--token` | `QUAT_REMOTE_TOKEN` | none | Token, if the engine requires one |
| `--timeout-secs` | `QUAT_MCP_TIMEOUT_SECS` | `15` | Per-request timeout |
| `--long-timeout-secs` | `QUAT_MCP_LONG_TIMEOUT_SECS` | `120` | Timeout for screenshots, batches, model spawn, scene load/save/clear, nav bake |
| `--max-reply-mb` | `QUAT_MCP_MAX_REPLY_MB` | `64` | Largest reply accepted from the engine |
| — | `QUAT_MCP_LOG` | `info` | Log filter (`debug`, `quat_mcp=trace`, …) |

Logs go to stderr; stdout carries the MCP protocol.

## Use with Claude Code

```sh
claude mcp add quat-3d \
  --env QUAT_REMOTE_PORT=7878 \
  -- /absolute/path/to/quat-3d/tools/quat-mcp/target/release/quat-mcp
```

With a token, add `--env QUAT_REMOTE_TOKEN=s3cret`. Use `--scope user` to make
the server available in every project, or `--scope project` to write it into
the repository's `.mcp.json` for the whole team. Check the connection with
`claude mcp list`, or `/mcp` inside a session.

## Use with Claude Desktop

Edit `claude_desktop_config.json` (Settings → Developer → Edit Config; on macOS
`~/Library/Application Support/Claude/claude_desktop_config.json`, on Windows
`%APPDATA%\Claude\claude_desktop_config.json`) and restart Claude Desktop:

```json
{
  "mcpServers": {
    "quat-3d": {
      "command": "/absolute/path/to/quat-3d/tools/quat-mcp/target/release/quat-mcp",
      "env": {
        "QUAT_REMOTE_PORT": "7878",
        "QUAT_REMOTE_TOKEN": "s3cret"
      }
    }
  }
}
```

Leave out `QUAT_REMOTE_TOKEN` if the engine runs without one. On Windows the
command ends in `quat-mcp.exe`.

## What the model gets

**Tools** — one per engine method in the v1 contract:

| Tool | Engine method | What it does |
|---|---|---|
| `engine_info` | `engine.info` | Frame, entity count, play state |
| `engine_methods` | `rpc.methods` | Methods the running engine implements |
| `engine_call` | any | Raw escape hatch for newer engine methods |
| `scene_components` | `scene.components` | Component types and whether they can be added |
| `scene_list` / `scene_get` | `scene.list` / `scene.get` | Find entities; read one in full |
| `scene_create` / `scene_update` | `scene.create` / `scene.update` | Create; rename / merge-update components |
| `scene_remove_components` | `scene.remove_components` | Remove components |
| `scene_destroy` / `scene_clear` | `scene.destroy` / `scene.clear` | Delete one subtree / everything |
| `scene_set_parent` | `scene.set_parent` | Re-parent (keeping the world transform by default) |
| `scene_batch` | `scene.batch` | Many commands in one frame, with `"@N"` back-references |
| `environment_get` / `environment_set` | `environment.*` | Sun angles and colour, volumetric clouds |
| `world_ground_height` | `world.ground_height` | Terrain height under `[x, z]` points |
| `camera_get` / `camera_set` / `camera_frame` | `camera.*` | Read, place or auto-frame the viewport camera |
| `screenshot` | `render.screenshot` | The viewport as an MCP **image** (PNG) plus a caption |
| `asset_list_models` / `asset_spawn_model` | `asset.*` | Find and spawn glTF models |
| `sim_play` / `sim_stop` | `sim.*` | Enter / leave play mode |
| `scene_save` / `scene_load` | `scene.save` / `scene.load` | Project-relative JSON scene files |
| `nav_bake` | `nav.bake` | Bake the navmesh for NPC agents |
| `debug_log` | `debug.log` | Engine log tail (levels, categories, Vulkan validation), incremental via a cursor |
| `debug_stats` | `debug.stats` | Frame times, draws, entities, physics bodies, nav agents, memory, clock, log error counts |
| `debug_pause` / `debug_resume` | `debug.pause` / `debug.resume` | Freeze / resume the simulation (rendering continues) |
| `debug_step` | `debug.step` | Advance N simulation frames of a fixed dt, reply when done |
| `debug_time_scale` | `debug.time_scale` | Slow motion / fast-forward (0.01–10×) |
| `debug_watch` | `debug.watch` | Live component snapshot of up to 64 entities (+ nav agent state) |
| `debug_raycast` | `debug.raycast` | Physics ray: hit entity, point, normal (play mode) |
| `debug_nav_path` | `debug.nav_path` | Navmesh path between two points, partial or not |
| `bt_leaves` | `bt.leaves` | Behaviour-tree node kinds and leaves with their parameters, defaults and ranges |
| `bt_get` / `bt_set` | `bt.get` / `bt.set` | Read an entity's tree (outline with stable node ids, issues, tree JSON); replace it (validated) |
| `bt_add_node` / `bt_remove_node` / `bt_move_node` / `bt_set_params` | `bt.*` | Edit the tree by node id; invalid results are refused with per-node errors |
| `bt_status` | `bt.status` | What each node returned on the last tick (Play) |
| `render_settings_get` / `render_settings_set` | `render.settings.get` / `render.settings.set` | Read / merge-update the renderer settings: exposure, bloom, TAA, shadows, sky light, clouds |
| `render_settings_reset` | `render.settings.reset` | Restore one group's (or every group's) defaults |
| `render_settings_schema` | `render.settings.schema` | Every tunable field with type, range, default, unit and hint (settings and material factors) |
| `material_get` / `material_set` | `material.get` / `material.set` | An entity's PBR factors (colour, roughness, metallic, clearcoat, glass, emission …); per-entity edits and reverts |
| `script_list` / `script_get` | `script.list` / `script.get` | Lua scripts with their status (errors with file:line); read one |
| `script_create` / `script_update` | `script.create` / `script.update` | Write a script (compiled first, syntax errors name the line); updates hot-reload while playing |
| `script_delete` / `script_check` | `script.delete` / `script.check` | Delete a script; compile a draft without saving |

Tool descriptions spell out units and conventions (metres, Y up, Euler degrees
in YXZ order, `"#rrggbb"` sRGB colours for primitives and linear `[r,g,b]` for
lights, entity references by id or unique name) and document every component
and its fields. Engine errors come back as tool errors with the engine's code
and message intact (for example `scene.update failed — engine error
invalid_params (-32602): point_light.radius: expected a number >= 0`), plus a
short hint, so the model can correct itself.

**Prompts** — `build_world` (inspect → plan → build with `scene_batch` →
screenshot → iterate), `inspect_scene` (survey and review the current
scene), `debug_session` (diagnose a problem: stats → logs → watch →
pause / step → raycast / nav path → conclude), `design_behavior` (an NPC
behaviour tree: leaves → plan → `bt_set` or edits → play → `bt_status` /
`debug_watch` → iterate), `tune_look` (screenshot → read settings and
schema → exposure first, then bloom, shadows, sky light and materials, one
change and one screenshot at a time) and `write_game_script` (read the Lua
reference → write → compile → attach → play → read the log → iterate). In
Claude Code they appear as `/mcp__quat-3d__build_world`.

**Resources** — `quat://scene/summary`: engine info, the first 500 entities,
the environment and the camera as JSON; `quat://docs/scripting`: the Lua
scripting reference (the engine's `docs/scripting.md`).

## Example session

> **You:** /mcp__quat-3d__build_world a small stone watchtower on a grassy hill at sunset
>
> **Claude:** *calls `scene_components`, `scene_list`, `environment_get`,
> `camera_get`, then writes a plan with coordinates* … *calls `scene_batch`
> with 24 commands: a `Hill` sphere scaled flat, a `Tower` parent with
> `Tower_Base`, `Tower_Shaft` and `Tower_Roof` (cone) children referenced as
> `"@1"`, a warm `point_light` in the window* … *calls `environment_set`
> `{"sun":{"elevation_deg":6,"azimuth_deg":250}}`, `camera_frame` and
> `screenshot`* — "The roof floats 0.3 m above the shaft; lowering it." —
> *calls `scene_update` on `Tower_Roof`, screenshots again* — "Done: the tower
> stands on the hill, lit from the window against an orange sky."

## Development

```sh
cargo fmt --check
cargo clippy --all-targets -- -D warnings
cargo test
```

The tests run without the engine: `tests/common/mod.rs` is a mock engine (a
tokio TCP server speaking the line protocol) used to test the client
(handshake and tokens, id matching, timeouts, reconnects, error mapping, size
limits) and the tools end to end through an in-process MCP client.

Layout:

| Path | What |
|---|---|
| `content/` | **Everything the model reads**, as Markdown: server instructions, one file per tool description, the component reference, prompt templates ([MiniJinja](https://docs.rs/minijinja)). See `content/README.md`. |
| `src/client/` | Engine JSON-RPC client: `mod.rs` (calls, reconnects, handshake), `connection.rs` (socket, reader task, in-flight calls), `error.rs` (error codes, `EngineError`) |
| `src/server/` | MCP server: `mod.rs` (handler, resource), tools grouped by area (`scene_tools.rs`, `world_tools.rs`, `sim_tools.rs`, `debug_tools.rs`, `behavior_tools.rs`, `tuning_tools.rs`, `script_tools.rs`, each its own rmcp router), `bt_outline.rs` (behaviour-tree outlines), `prompts.rs`, `results.rs` |
| `src/params.rs`, `src/params/` | Typed tool inputs — their doc comments are the JSON schemas the model reads (`debug.rs`, `behavior.rs`, `tuning.rs`, `script.rs` per area) |
| `src/content.rs` | Embeds `content/` and renders prompt templates |
| `src/config.rs` | Flags / environment |

To change what the model is told, edit the Markdown — no prose lives in Rust
string literals (error messages excepted). A new tool is a function with
`#[doc = include_str!("../../content/tools/<name>.md")]` + `#[tool]`; a test
fails if a tool and its Markdown file don't match one-to-one.

When the engine contract in `docs/remote-control.md` changes, update it there
first, then the matching tool here.
