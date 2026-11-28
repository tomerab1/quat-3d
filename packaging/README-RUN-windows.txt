quat-3d - Windows (x64)
=======================

Run
---
Double-click game.exe (a console window shows the log), or from a terminal:

    game.exe

The editor opens with a showcase scene. Hold the right mouse button in the
viewport and use WASD to fly; W/E/R switch the gizmo between move, rotate
and scale.

Requirements
------------
- Windows 10/11 x64 and a GPU driver with Vulkan 1.3 and
  VK_EXT_descriptor_buffer (current NVIDIA, AMD or Intel drivers). The driver
  installs vulkan-1.dll.
- Everything else (libraries, the MSVC runtime) is in this folder: keep the
  DLLs, assets\ and cooked\ next to game.exe.

Options (environment variables)
-------------------------------
cmd.exe:     set QUAT_TERRAIN=1 && game.exe
PowerShell:  $env:QUAT_TERRAIN = "1"; .\game.exe

    QUAT_TERRAIN=1                        add a procedural terrain tile
    QUAT_PHYSICS=1                        physics demo scene
    QUAT_CHARACTER=1                      character-controller demo
    QUAT_GLTF=C:\models\model.glb         load a glTF/GLB model instead
    QUAT_HOST=<port> / QUAT_JOIN=<ip:port>  host or join a networked session
    QUAT_REMOTE=1                         remote-control endpoint for AI tools (port 7878)
    QUAT_NO_UI=1                          no editor UI
    QUAT_NO_VSYNC=1                       uncapped frame rate
    QUAT_NO_AUDIO=1                       mute (no playback device)
    QUAT_ASSET_ROOT=<dir>                 data directory (assets\ + cooked\shaders\);
                                          default: the folder that holds game.exe

AI assistants (Claude Code)
---------------------------
quat-mcp.exe, next to the game, is the MCP server that lets Claude Code
drive the running game. Start the game with the endpoint, then register
the server by its absolute path (the token needs no setup). In PowerShell,
from this folder:

    $env:QUAT_REMOTE=1; .\game.exe
    claude mcp add quat-3d -- "$PWD\quat-mcp.exe" --project-root "$PWD"

--project-root is where asset_fetch saves downloaded models (this folder).

Troubleshooting
---------------
- "vulkan-1.dll was not found": install or update the GPU driver.
- "no data directory ... found": assets\ and cooked\ must stay next to
  game.exe (or set QUAT_ASSET_ROOT to the folder that contains them).
- Vulkan device creation fails / no suitable GPU: update the GPU driver; it
  must support Vulkan 1.3 and VK_EXT_descriptor_buffer.
- "Windows protected your PC" (SmartScreen): the build is not code-signed;
  choose "More info" -> "Run anyway".
