# quat-3d plugin for Claude Code

Drive a running quat-3d engine from Claude Code: the
[quat-mcp](../README.md) MCP server plus workflow skills.

| Skill | For |
|---|---|
| `quat-3d:build-level` | Build or extend a level: terrain, models, lights, sky, VFX |
| `quat-3d:tune-look` | Exposure, bloom, shadows, AO, sky light, one change per screenshot |
| `quat-3d:author-material-shader` | PBR materials: metal, plastic, car paint, glass, emission |
| `quat-3d:write-gameplay-script` | Lua gameplay scripts: write, compile, attach, play, read the log |
| `quat-3d:design-npc-behaviour` | NPC behaviour trees: plan, build, play, watch node status |
| `quat-3d:debug-physics-glitch` | Objects falling through, jittering, tunnelling, stuck |
| `quat-3d:playtest` | Run the level and report script errors, stuck NPCs, hitches |
| `quat-3d:inspect-scene` | Survey and review a scene without changing it |
| `quat-3d:debug-session` | General debugging: stats, logs, watch, pause and step |

## Install

1. Build quat-mcp and put it on your PATH (or note the binary's path):

   ```sh
   cargo install --path tools/quat-mcp
   ```

2. In Claude Code:

   ```
   /plugin marketplace add tomerab1/quat-3d
   /plugin install quat-3d@quat-3d
   ```

   When the plugin is enabled, Claude Code asks for two options:
   `quat_mcp_command` (default `quat-mcp`; give the absolute path of
   `tools/quat-mcp/target/release/quat-mcp` if it is not on your PATH) and
   `engine_port` (default `7878`, the engine's `QUAT_REMOTE` value).

3. Start the engine with the remote-control endpoint
   (`QUAT_REMOTE=7878 ./build/release/game`). quat-mcp connects on the first
   tool call. The plugin holds no secrets: quat-mcp reads the engine's
   session token from the token file the engine writes, or from
   `QUAT_REMOTE_TOKEN` in your environment.

The skills are copies of `tools/quat-mcp/content/skills/` (a test keeps them
identical). The plugin starts quat-mcp with `--skills=false --prompts=false`
because the skills are already installed as files.
