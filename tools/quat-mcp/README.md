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

Tool descriptions spell out units and conventions (metres, Y up, Euler degrees
in YXZ order, `"#rrggbb"` sRGB colours for primitives and linear `[r,g,b]` for
lights, entity references by id or unique name) and document every component
and its fields. Engine errors come back as tool errors with the engine's code
and message intact (for example `scene.update failed — engine error
invalid_params (-32602): point_light.radius: expected a number >= 0`), plus a
short hint, so the model can correct itself.

**Prompts** — `build_world` (inspect → plan → build with `scene_batch` →
screenshot → iterate) and `inspect_scene` (survey and review the current
scene). In Claude Code they appear as `/mcp__quat-3d__build_world`.

**Resource** — `quat://scene/summary`: engine info, the first 500 entities,
the environment and the camera as JSON.

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

Layout: `src/client.rs` (engine JSON-RPC client), `src/server.rs` (tools,
prompts, resource), `src/params.rs` (typed tool inputs — their doc comments
are the JSON schemas the model reads), `src/config.rs` (flags / environment).

When the engine contract in `docs/remote-control.md` changes, update it there
first, then the matching tool here.
