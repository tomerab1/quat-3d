quat-3d - macOS (Apple Silicon)
===============================

Run
---
The app is ad-hoc signed but not notarized, so the first launch of a
downloaded copy is blocked by Gatekeeper ("Apple could not verify ..." /
"cannot be opened"). Allow it once, either way:

  - Double-click Quat3D.app, dismiss the dialog, then open
    System Settings -> Privacy & Security, scroll to Security and click
    "Open Anyway" next to Quat3D, and confirm (macOS 15 and later; on
    macOS 14, right-click the app -> Open -> Open also works).
  - Or, in Terminal, remove the download quarantine flag (also fixes an
    "is damaged and can't be opened" message):

        xattr -dr com.apple.quarantine Quat3D.app

After that it opens normally with a double-click.

The editor opens with a showcase scene. Hold the right mouse button in the
viewport and use WASD to fly; W/E/R switch the gizmo between move, rotate
and scale.

Requirements
------------
- An Apple Silicon Mac (arm64), macOS @MINOS@ or later. Vulkan runs on Metal
  through MoltenVK, which ships inside the app together with the Vulkan
  loader - nothing to install.

Options (environment variables)
-------------------------------
Launch the executable inside the bundle from Terminal to use them (and to
see the log):

    QUAT_TERRAIN=1 ./Quat3D.app/Contents/MacOS/game

    QUAT_DEMO=mountain                    the mountain-forest showcase
    QUAT_TIME=2026-06-21T21:30+02:00      a world clock at that date and time
                                          drives the sun; QUAT_LATLON=<lat>,<lon>
                                          sets the place (default Paris),
                                          QUAT_TIME_SCALE=<n> its speed (0 = frozen)
    QUAT_HDRI=hdri/sky.hdr                light the scene with a Radiance .hdr
                                          panorama (path relative to
                                          Quat3D.app/Contents/Resources)
    QUAT_TERRAIN=1                        add a procedural terrain tile
    QUAT_PHYSICS=1                        physics demo scene
    QUAT_CHARACTER=1                      character-controller demo
    QUAT_GLTF=/path/model.glb             load a glTF/GLB model instead
    QUAT_HOST=<port> / QUAT_JOIN=<ip:port>  host or join a networked session
    QUAT_REMOTE=1                         remote-control endpoint for AI tools (port 7878)
    QUAT_NO_UI=1                          no editor UI
    QUAT_NO_VSYNC=1                       uncapped frame rate
    QUAT_NO_AUDIO=1                       mute (no playback device)
    QUAT_ASSET_ROOT=<dir>                 data directory (assets/ + cooked/shaders/);
                                          default: Quat3D.app/Contents/Resources

AI assistants (Claude Code)
---------------------------
Quat3D.app/Contents/MacOS/quat-mcp is the MCP server that lets Claude Code
drive the running game. Run `xattr -dr com.apple.quarantine Quat3D.app`
once so it may start. Then start the game with the endpoint, and register
the server by its absolute path (the token needs no setup):

    QUAT_REMOTE=1 ./Quat3D.app/Contents/MacOS/game
    claude mcp add quat-3d -- "$PWD/Quat3D.app/Contents/MacOS/quat-mcp" \
      --project-root "$PWD/Quat3D.app/Contents/Resources"

--project-root is where asset_fetch saves downloaded models.

Or install the quat-3d plugin, which adds the workflow skills: in Claude
Code run `/plugin marketplace add tomerab1/quat-3d` and
`/plugin install quat-3d@quat-3d`, then give its quat_mcp_command option
the absolute path above, and its project_root option the --project-root
value.

Troubleshooting
---------------
- Gatekeeper refuses to open the app: see "Run" above.
- Nothing happens on launch: run Quat3D.app/Contents/MacOS/game from
  Terminal to read the error; VK_LOADER_DEBUG=driver shows which Vulkan
  driver the loader picks.
- "no data directory ... found": the bundle was modified; extract it again.
