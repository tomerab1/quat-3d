# quat-3d plugin for Claude Code

Drive a running quat-3d engine from Claude Code: the
[quat-mcp](../README.md) MCP server plus workflow skills.

| Skill | For |
|---|---|
| `quat-3d:build-level` | Build or extend a level: terrain, models, lights, sky, VFX |
| `quat-3d:tune-look` | Exposure, bloom, shadows, AO, sky light, one change per screenshot |
| `quat-3d:author-material-shader` | PBR materials (metal, plastic, car paint, glass, emission) and Slang material shaders |
| `quat-3d:write-gameplay-script` | Lua gameplay scripts: write, compile, attach, play, read the log |
| `quat-3d:design-npc-behaviour` | NPC behaviour trees: plan, build, play, watch node status |
| `quat-3d:set-weather` | Wind, clouds, rain or snow and wet / snowy surfaces, checked with screenshots |
| `quat-3d:debug-physics-glitch` | Objects falling through, jittering, tunnelling, stuck |
| `quat-3d:playtest` | Run the level and report script errors, stuck NPCs, hitches |
| `quat-3d:inspect-scene` | Survey and review a scene without changing it |
| `quat-3d:debug-session` | General debugging: stats, logs, watch, pause and step |

## Install

1. Get quat-mcp, either way:

   - **From a release.** Every release archive bundles quat-mcp next to
     the game — no Rust toolchain needed:

     | OS | quat-mcp | Data folder (`--project-root`) |
     |---|---|---|
     | Linux | `quat-mcp` next to `game` | the folder that holds `game` |
     | Windows | `quat-mcp.exe` next to `game.exe` | the folder that holds `game.exe` |
     | macOS | `Quat3D.app/Contents/MacOS/quat-mcp` | `Quat3D.app/Contents/Resources` |

     On macOS run `xattr -dr com.apple.quarantine Quat3D.app` once so the
     bundled binary may start (the release's `README-RUN` explains why).
   - **From source.** Build it and put it on your PATH:

     ```sh
     cargo install --path tools/quat-mcp
     ```

2. In Claude Code:

   ```
   /plugin marketplace add tomerab1/quat-3d
   /plugin install quat-3d@quat-3d
   ```

   When the plugin is enabled, Claude Code asks for three options:

   - `quat_mcp_command` — default `quat-mcp` (on your PATH). For a release,
     give the **absolute path** of the bundled binary from the table, e.g.
     `/home/me/quat-3d-linux/quat-mcp`,
     `C:\Games\quat-3d\quat-mcp.exe` or
     `/Applications/Quat3D.app/Contents/MacOS/quat-mcp`; for a source build
     not on your PATH, the absolute path of
     `tools/quat-mcp/target/release/quat-mcp`. A relative path resolves
     against the directory Claude Code was started in. The default
     `quat-mcp` needs `~/.cargo/bin` on the PATH of the Claude Code process,
     which an app launched from the desktop often lacks: if `/mcp` shows the
     server failed, give an absolute path.
   - `engine_port` — default `7878`, the engine's `QUAT_REMOTE` value.
   - `project_root` — where `asset_fetch` saves downloaded models (into its
     `assets/`, where the engine finds them). Recommended for a release: the
     **data folder** from the table, the same as `--project-root` in the
     release's `README-RUN`. For a source checkout, the checkout's root.
     Empty (the default) means the directory Claude Code starts quat-mcp
     in.

3. Start the engine with the remote-control endpoint (`QUAT_REMOTE=7878
   ./build/release/game` from source; for a release, as its `README-RUN`
   shows, e.g. `QUAT_REMOTE=1 ./game`, which listens on 7878). quat-mcp
   connects on the first tool call. The plugin holds no secrets: quat-mcp
   reads the engine's session token from the token file the engine writes,
   or from `QUAT_REMOTE_TOKEN` in your environment.

The skills are copies of `tools/quat-mcp/content/skills/` (a test keeps them
identical). The plugin starts quat-mcp with `--skills=false --prompts=false`
because the skills are already installed as files.
