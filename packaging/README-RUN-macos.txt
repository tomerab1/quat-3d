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
  Terminal to read the error. The "[rhi] driver:" line at startup names the
  Vulkan driver that loaded and its version (VK_LOADER_DEBUG=driver shows
  how the loader chose it).
- A Vulkan SDK in your Terminal environment (LunarG's setup-env.sh sets
  VK_DRIVER_FILES / VK_ICD_FILENAMES) makes the app use the SDK's MoltenVK
  instead of the bundled one; the "[rhi] driver:" line then names that
  variable. Supported: MoltenVK 1.4.0 / Vulkan SDK 1.4.341 or newer. To use
  the bundled MoltenVK, launch it without them:

      env -u VK_DRIVER_FILES -u VK_ICD_FILENAMES ./Quat3D.app/Contents/MacOS/game
- "no data directory ... found": the bundle was modified; extract it again.
