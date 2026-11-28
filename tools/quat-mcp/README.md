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
QUAT_REMOTE=7878 QUAT_REMOTE_TOKEN=s3cret ./build/release/game   # choose the token yourself
```

quat-mcp connects lazily on the first tool call and reconnects automatically
if the engine is restarted, so the engine and the AI client can be started in
any order.

### Session token

The engine always requires a session token. Unless you give it one with
`QUAT_REMOTE_TOKEN`, it generates a random 256-bit token for each run and
writes it to a token file only your user can read; quat-mcp reads that file,
so **no token configuration is needed**. It reads the file again on every
reconnect, so restarting the engine (which changes the token) needs no
quat-mcp restart.

| Platform | Token file (`<port>` = the engine's port) |
|---|---|
| Linux | `$XDG_RUNTIME_DIR/quat-3d/remote-<port>.token`, else `~/.local/state/quat-3d/remote-<port>.token` |
| macOS | `~/Library/Application Support/quat-3d/remote-<port>.token` |
| Windows | `%LOCALAPPDATA%\quat-3d\remote-<port>.token` |

Set `QUAT_REMOTE_TOKEN_FILE` to the same path for both to move it (for
example when the engine and quat-mcp see different `XDG_RUNTIME_DIR`
values), or `QUAT_REMOTE_TOKEN` to the same value for both to skip the file.
See [Session token](../../docs/remote-control.md#session-token) in the
protocol docs.

## Configuration

Every option is a flag or an environment variable:

| Flag | Environment | Default | Meaning |
|---|---|---|---|
| `--port` | `QUAT_REMOTE_PORT` | `7878` | Engine port (the engine's `QUAT_REMOTE` value) |
| `--host` | `QUAT_REMOTE_HOST` | `127.0.0.1` | Engine host |
| `--token` | `QUAT_REMOTE_TOKEN` | none | Token, if the engine was started with `QUAT_REMOTE_TOKEN` |
| `--token-file` | `QUAT_REMOTE_TOKEN_FILE` | per-user file for `--port` | Where to read the engine's token when `--token` is not given |
| `--timeout-secs` | `QUAT_MCP_TIMEOUT_SECS` | `15` | Per-request timeout |
| `--long-timeout-secs` | `QUAT_MCP_LONG_TIMEOUT_SECS` | `120` | Timeout for screenshots, batches, model spawn, scene load/save/clear, nav bake |
| `--max-reply-mb` | `QUAT_MCP_MAX_REPLY_MB` | `64` | Largest reply accepted from the engine |
| `--project-root` | `QUAT_MCP_PROJECT_ROOT` | current directory | The quat-3d checkout `asset_fetch` downloads into (`assets/meshes/polyhaven/`) |
| `--polyhaven-api` | `QUAT_MCP_POLYHAVEN_API` | `https://api.polyhaven.com` | Poly Haven API base URL |
| `--asset-max-mb` | `QUAT_MCP_ASSET_MAX_MB` | `512` | Largest model download `asset_fetch` accepts |
| `--skills` | `QUAT_MCP_SKILLS` | `true` | Serve the workflow skills over the MCP skills extension |
| `--prompts` | `QUAT_MCP_PROMPTS` | `true` | Serve the workflows as MCP prompts |
| — | `QUAT_MCP_LOG` | `info` | Log filter (`debug`, `quat_mcp=trace`, …) |

Logs go to stderr; stdout carries the MCP protocol.

## Use with Claude Code

The easiest way is the **quat-3d plugin**: it registers quat-mcp and installs
the workflow skills. Put quat-mcp on your PATH first
(`cargo install --path tools/quat-mcp`), then in Claude Code:

```
/plugin marketplace add tomerab1/quat-3d
/plugin install quat-3d@quat-3d
```

Claude Code asks for the quat-mcp binary (default `quat-mcp`) and the engine
port (default `7878`) when the plugin is enabled. The skills then appear as
`/quat-3d:build-level` and so on, and Claude uses them on its own when a task
matches. See [`plugin/README.md`](plugin/README.md).

Without the plugin, add the server by hand:

```sh
claude mcp add quat-3d \
  --env QUAT_REMOTE_PORT=7878 \
  -- /absolute/path/to/quat-3d/tools/quat-mcp/target/release/quat-mcp
```

If you started the engine with `QUAT_REMOTE_TOKEN`, add
`--env QUAT_REMOTE_TOKEN=s3cret`; otherwise the token file is found
automatically. Use `--scope user` to make
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
        "QUAT_REMOTE_PORT": "7878"
      }
    }
  }
}
```

Add `"QUAT_REMOTE_TOKEN": "s3cret"` only if you started the engine with
`QUAT_REMOTE_TOKEN`; otherwise quat-mcp reads the engine's token file. On
Windows the command ends in `quat-mcp.exe`.

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
| `time_get` / `time_set` / `time_skip` | `time.*` | World clock: date, time, place and speed; the real Sun and Moon, sunrise / sunset, Moon phase |
| `world_ground_height` | `world.ground_height` | Terrain height under `[x, z]` points |
| `terrain_status` | `terrain.status` | Whether terrain tiles are still generating; loaded tiles and bounds |
| `world_place_on_ground` | `world.place_on_ground` | Rest an entity's bounds on the terrain or colliders below it, optionally tilted to the slope |
| `camera_get` / `camera_set` / `camera_frame` | `camera.*` | Read, place or auto-frame the viewport camera |
| `camera_rig_get` / `camera_rig_set` / `camera_rig_frame` | `camera.rig.*` | Read, drive (optionally snapping) or frame an entity's orbit / strategy camera rig |
| `input_actions` / `input_inject` / `input_bind` / `input_context` | `input.*` | List input actions and contexts; make an action read a value for N frames (playtests); rebind (saved per user); push / pop contexts |
| `screenshot` | `render.screenshot` | The viewport (or an explicit camera, leaving the viewport alone) as an MCP **image** (PNG) plus a caption |
| `asset_list_models` / `asset_spawn_model` | `asset.*` | Find and spawn glTF models |
| `asset_search` / `asset_fetch` | — (Poly Haven API) | Search CC0 models on Poly Haven; download one (glTF + includes, size-capped, checksummed, with `LICENSE.md` / `polyhaven.json`) into `assets/meshes/polyhaven/<id>/`, cooked with `quat-cook` when it is on PATH |
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
| `physics_get` / `physics_set` | `physics.get` / `physics.set` | A body's live state (velocities, mass, sleeping, motion type, layer, tuning); change velocities, motion type, gravity factor, friction, restitution, damping, wake / sleep (play mode) |
| `physics_apply` | `physics.apply` | Impulse, force, angular impulse or torque on a dynamic body, optionally at a point |
| `physics_get_gravity` / `physics_set_gravity` | `physics.get_gravity` / `physics.set_gravity` | The scene's one gravity setting (bodies + characters, saved with the scene) |
| `physics_raycast_all` / `physics_overlap` / `physics_shape_cast` | `physics.raycast_all` / `.overlap` / `.shape_cast` | Every hit along a ray, bodies a sphere / box / capsule overlaps, a swept shape's hits — with layer masks |
| `events_poll` | `events.poll` | Contact and trigger events (who hit whom, where, how hard), incremental via a cursor |
| `character_move` | `character.move` | Walk a character controller at a velocity or to a point, jump |
| `weather_get` / `weather_set` | `weather.get` / `weather.set` | The scene's weather: wind, cloud cover, rain / snow, wetness, snow cover, temperature; wind velocity at points |
| `script_list` / `script_get` | `script.list` / `script.get` | Lua scripts with their status (errors with file:line); read one |
| `script_create` / `script_update` | `script.create` / `script.update` | Write a script (compiled first, syntax errors name the line); updates hot-reload while playing |
| `script_delete` / `script_check` | `script.delete` / `script.check` | Delete a script; compile a draft without saving |
| `shader_list` | `shader.list` | Material shaders (built-in + authored) and their compile state |
| `shader_get` | `shader.get` | One material shader's source, state and last diagnostics |
| `shader_template` | `shader.template` | The starter material shader module |
| `shader_create` / `shader_update` | `shader.create` / `shader.update` | Write + compile a material shader; errors as `file:line:column` with a caret; a failed update keeps the old version |
| `shader_delete` | `shader.delete` | Delete a material shader; entities using it fall back to the built-in |
| `project_info` | `project.info` | The open game project (`game.json`), its executable name and the host target |
| `project_build` / `project_build_status` | `project.build` / `project.build_status` | Package the game for the host OS in the background; poll progress, log tail and the archive path |

Tool descriptions spell out units and conventions (metres, Y up, Euler degrees
in YXZ order, `"#rrggbb"` sRGB colours for primitives and linear `[r,g,b]` for
lights, entity references by id or unique name) and document every component
and its fields. Engine errors come back as tool errors with the engine's code
and message intact (for example `scene.update failed — engine error
invalid_params (-32602): point_light.radius: expected a number >= 0`), plus a
short hint, so the model can correct itself.

**Workflows** — step-by-step guides with verification steps and pitfalls,
written once as [Agent Skills](https://agentskills.io) in
`content/skills/<name>/SKILL.md`:

| Workflow | For |
|---|---|
| `build-level` | Build or extend a level: inspect, plan in metres, `scene_batch`, screenshot, iterate |
| `tune-look` | Exposure first, then bloom, shadows, AO, sky light and materials, one change per screenshot |
| `author-material-shader` | PBR materials (metal, plastic, car paint, glass, emission) from reference values, and Slang material shaders with `shader_*` |
| `write-gameplay-script` | Lua scripts: read the reference, write, compile, attach, play, read the log |
| `design-npc-behaviour` | Behaviour trees: leaves, plan, `bt_set`, play, `bt_status` / `debug_watch` |
| `set-weather` | Wind, gusts, cloud cover, rain / snow, wet and snowy surfaces, checked with screenshots |
| `find-performance-problem` | Profiling: baseline `profile_capture`, CPU- or GPU-bound, locate the zone, one fix at a time, before / after numbers |
| `debug-physics-glitch` | Falling through, jitter, tunnelling, stuck bodies: pause, step, watch, raycast |
| `playtest` | Run the level and report script errors, stuck NPCs, objects leaving the world, hitches |
| `inspect-scene` | Survey and review the scene without changing it |
| `debug-session` | General debugging: stats, logs, watch, pause / step, raycasts, nav paths |

Each skill lists the quat-mcp tools it uses in its frontmatter
(`metadata.quat-mcp-tools`). quat-mcp offers them three ways:

- **MCP skills extension** ([SEP-2640](https://modelcontextprotocol.io/seps/2640-skills-extension),
  `io.modelcontextprotocol/skills`): `skills/list` and `skills/get` return
  each skill's frontmatter and a digest manifest, and every file is a
  resource at `skill://<name>/SKILL.md`. Clients with skills support (Claude
  Code 2.1.332+, beta) load them as `quat-3d:build-level`.
- **MCP prompts** of the same names for clients without skills support,
  with arguments such as `description` or `goal` (from
  `content/prompts/<name>.md`). In Claude Code they appear as
  `/mcp__quat-3d__build-level`.
- **The Claude Code plugin** in `plugin/`, which installs them as plugin
  skills (and runs quat-mcp with `--skills=false --prompts=false` so they are
  not listed twice).

The server instructions name every workflow and its `skill://` URI.

**Renamed prompts (A12).** The prompts now carry the skill names, so the old
ones are gone; asking for an old name returns an error that lists the new
ones. The arguments are unchanged.

| Old prompt | New prompt / skill |
|---|---|
| `build_world` | `build-level` |
| `inspect_scene` | `inspect-scene` |
| `debug_session` | `debug-session` |
| `tune_look` | `tune-look` |
| `design_behavior` | `design-npc-behaviour` |
| `write_game_script` | `write-gameplay-script` |
| `write_material_shader` | `author-material-shader` |

In Claude Code, `/mcp__quat-3d__build_world` becomes
`/mcp__quat-3d__build-level` (or `/quat-3d:build-level` with the plugin).
New: `debug-physics-glitch` and `playtest`.

**Resources** — `quat://scene/summary`: engine info, the first 500 entities,
the environment and the camera as JSON; `quat://docs/scripting`: the Lua
scripting reference (the engine's `docs/scripting.md`); and the skill files
(`skill://<name>/SKILL.md`).

## Example session

> **You:** /quat-3d:build-level a small stone watchtower on a grassy hill at sunset
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
(handshake, tokens and the token file, id matching, timeouts, reconnects, error mapping, size
limits) and the tools end to end through an in-process MCP client.

Layout:

| Path | What |
|---|---|
| `content/` | **Everything the model reads**, as Markdown: server instructions, one file per tool description, the component reference, the workflow skills and their prompt templates ([MiniJinja](https://docs.rs/minijinja)). See `content/README.md`. |
| `src/client/` | Engine JSON-RPC client: `mod.rs` (calls, reconnects, handshake), `connection.rs` (socket, reader task, in-flight calls), `error.rs` (error codes, `EngineError`), `token_file.rs` (the engine's session token file) |
| `src/server/` | MCP server: `mod.rs` (handler, resource), tools grouped by area (`scene_tools.rs`, `world_tools.rs`, `sim_tools.rs`, `debug_tools.rs`, `behavior_tools.rs`, `tuning_tools.rs`, `script_tools.rs`, `shader_tools.rs`, `project_tools.rs`, each its own rmcp router), `bt_outline.rs` (behaviour-tree outlines), `results.rs` |
| `src/skills/` | The workflows: `mod.rs` (skill catalog: `skills/list`, `skills/get`, `skill://` resources), `frontmatter.rs` (Agent Skills rules), `prompts.rs` (prompts generated from the skills), `embedded.rs` (the compiled-in files), `tests.rs` (sync tests) |
| `plugin/` | The Claude Code plugin: `.claude-plugin/plugin.json`, `.mcp.json`, and `skills/`, a copy of `content/skills/` |
| `src/params.rs`, `src/params/` | Typed tool inputs — their doc comments are the JSON schemas the model reads (`debug.rs`, `behavior.rs`, `tuning.rs`, `script.rs`, `shader.rs`, `project.rs` per area) |
| `src/content.rs` | Embeds `content/` and renders the instructions and document templates |
| `src/config.rs` | Flags / environment |

To change what the model is told, edit the Markdown — no prose lives in Rust
string literals (error messages excepted). A new tool is a function with
`#[doc = include_str!("../../content/tools/<name>.md")]` + `#[tool]`; a test
fails if a tool and its Markdown file don't match one-to-one.

A new workflow is a directory `content/skills/<name>/` with a `SKILL.md`
(frontmatter `name`, `description`, `compatibility`, and
`metadata.quat-mcp-tools` listing every tool it names), a prompt template
`content/prompts/<name>.md`, one line each in `src/skills/embedded.rs`, and
a refreshed plugin copy (`rm -r plugin/skills && cp -R content/skills
plugin/skills`). The sync tests in `src/skills/tests.rs` fail when a skill
names a tool or resource that does not exist, leaves a used tool out of its
metadata, breaks the Agent Skills rules, lacks a prompt, or when the plugin
copy or manifests drift.

When the engine contract in `docs/remote-control.md` changes, update it there
first, then the matching tool here.
