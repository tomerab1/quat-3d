quat-3d - Linux (x86-64)
========================

Run
---
    ./game

The editor opens with a showcase scene. Hold the right mouse button in the
viewport and use WASD to fly; W/E/R switch the gizmo between move, rotate
and scale. Run it from a terminal to see its log.

Requirements
------------
- A Vulkan 1.3 driver with VK_EXT_descriptor_buffer - a current NVIDIA
  driver, or Mesa for AMD/Intel (Debian/Ubuntu: mesa-vulkan-drivers) - and
  the Vulkan loader (libvulkan1). Check with `vulkaninfo --summary`
  (package vulkan-tools).
- glibc 2.39 or newer (built on Ubuntu 24.04): Ubuntu 24.04+, Debian 13+,
  Fedora 40+, or any distribution of that age. Older systems fail at start
  with "version `GLIBC_2.39' not found". Check with `ldd --version`.
- X11 or Wayland. The C++ runtime (libc++) ships in lib/ and is found through
  the game's RUNPATH ($ORIGIN/lib): keep the folder together.

Options (environment variables)
-------------------------------
    QUAT_TERRAIN=1 ./game                 add a procedural terrain tile
    QUAT_PHYSICS=1 ./game                 physics demo scene
    QUAT_CHARACTER=1 ./game               character-controller demo
    QUAT_GLTF=/path/model.glb ./game      load a glTF/GLB model instead
    QUAT_HOST=<port> / QUAT_JOIN=<ip:port>  host or join a networked session
    QUAT_REMOTE=1                         remote-control endpoint for AI tools (port 7878)
    QUAT_NO_UI=1                          no editor UI
    QUAT_NO_VSYNC=1                       uncapped frame rate
    QUAT_NO_AUDIO=1                       mute (no playback device)
    QUAT_ASSET_ROOT=<dir>                 data directory (assets/ + cooked/shaders/);
                                          default: the folder that holds `game`

AI assistants (Claude Code)
---------------------------
quat-mcp, next to the game, is the MCP server that lets Claude Code drive
the running game. Start the game with the endpoint, then register the
server by its absolute path (the token needs no setup):

    QUAT_REMOTE=1 ./game
    claude mcp add quat-3d -- "$PWD/quat-mcp" --project-root "$PWD"

--project-root is where asset_fetch saves downloaded models (this folder).

Troubleshooting
---------------
- "no data directory ... found": assets/ and cooked/ must stay next to
  `game` (or point QUAT_ASSET_ROOT at the folder that contains them).
- "error while loading shared libraries: libvulkan.so.1": install the Vulkan
  loader and a driver (Debian/Ubuntu: sudo apt install libvulkan1 mesa-vulkan-drivers).
- Vulkan device creation fails / no suitable GPU: update the GPU driver; it
  must support Vulkan 1.3 and VK_EXT_descriptor_buffer. Without a GPU, Mesa's
  software driver works (slowly):
      VK_ICD_FILENAMES=/usr/share/vulkan/icd.d/lvp_icd.json ./game
  (the file is lvp_icd.x86_64.json on some distributions).
- No sound: check the default playback device; QUAT_NO_AUDIO=1 runs muted.
