quat-3d - macOS (Apple Silicon)
===============================

Run
---
The first time: right-click (or Control-click) Quat3D.app -> Open, then
confirm with Open. The app is ad-hoc signed but not notarized, so a plain
double-click is refused by Gatekeeper ("cannot be opened because the
developer cannot be verified"); after the first right-click -> Open it
starts normally. (On recent macOS versions you may instead have to allow it
under System Settings -> Privacy & Security -> "Open Anyway".) If macOS says
the app "is damaged", remove the download quarantine flag:

    xattr -dr com.apple.quarantine Quat3D.app

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

Troubleshooting
---------------
- Gatekeeper refuses to open the app: see "Run" above.
- Nothing happens on launch: run Quat3D.app/Contents/MacOS/game from
  Terminal to read the error; VK_LOADER_DEBUG=driver shows which Vulkan
  driver the loader picks.
- "no data directory ... found": the bundle was modified; extract it again.
