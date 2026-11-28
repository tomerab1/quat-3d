@NAME@ - Windows (x64)

Run
---
Double-click @EXE@.exe.

Made with the quat-3d engine.

Requirements
------------
- Windows 10/11 x64 and a GPU driver with Vulkan 1.3 and
  VK_EXT_descriptor_buffer (current NVIDIA, AMD or Intel drivers). The driver
  installs vulkan-1.dll.
- Everything else (libraries, the MSVC runtime) is in this folder: keep the
  DLLs, assets\, cooked\, game.json and manifest.json next to @EXE@.exe.

Windows SmartScreen may warn that the publisher is unknown (the build is not
signed yet): choose "More info" -> "Run anyway".

Troubleshooting
---------------
- Nothing happens: read its log - in cmd, run
      @EXE@.exe > log.txt 2>&1
  and open log.txt.
- "vulkan-1.dll was not found" / no suitable GPU: update the GPU driver.
