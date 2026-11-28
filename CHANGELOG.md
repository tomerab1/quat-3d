# Changelog

Each release's notes are in [`docs/releases/`](docs/releases/), newest first.
They are also the text of the matching
[GitHub Release](https://github.com/tomerab1/quat-3d/releases), where the
downloads are. [`docs/RELEASING.md`](docs/RELEASING.md) covers how releases
are cut.

| Version | Notes | Summary |
|---|---|---|
| v0.1.0 | [`docs/releases/v0.1.0.md`](docs/releases/v0.1.0.md) | First release: engine, editor, MCP server with workflow skills and a Claude Code plugin, Lua scripting, weather and a real-time sky clock, SSR and HDRI lighting, a cinematic camera, physics control over MCP, profiling, input action maps and a strategy camera rig, game packaging, the engine SDK (`find_package(quat3d)`), Linux / Windows / macOS builds with quat-mcp bundled, and the Coin Collector sample game |

## Breaking changes

### v0.1.0

The quat-mcp MCP prompts were renamed to match the workflow skills (#83). The
arguments are unchanged. This matters only to anyone who used quat-mcp from
the repository before #83.

| Old prompt | New prompt |
|---|---|
| `build_world` | `build-level` |
| `tune_look` | `tune-look` |
| `write_material_shader` | `author-material-shader` |
| `write_game_script` | `write-gameplay-script` |
| `design_behavior` | `design-npc-behaviour` |
| `inspect_scene` | `inspect-scene` |
| `debug_session` | `debug-session` |

`debug-physics-glitch`, `playtest`, `set-weather` and
`find-performance-problem` are new.
