@NAME@ - Linux (x86-64)

Run
---
    ./@EXE@

Made with the quat-3d engine. Run it from a terminal to see its log.

Requirements
------------
- A Vulkan 1.3 driver with VK_EXT_descriptor_buffer - a current NVIDIA
  driver, or Mesa for AMD/Intel (Debian/Ubuntu: mesa-vulkan-drivers) - and
  the Vulkan loader (libvulkan1). Check with `vulkaninfo --summary`
  (package vulkan-tools).
- glibc 2.39 or newer (Ubuntu 24.04+, Debian 13+, Fedora 40+).
- X11 or Wayland. The C++ runtime (libc++) ships in lib/: keep the folder
  together.

Options (environment variables)
-------------------------------
    QUAT_NO_AUDIO=1       mute (no playback device)
    QUAT_NO_VSYNC=1       uncapped frame rate

Troubleshooting
---------------
- "no data directory ... found" / "no game.json": assets/, cooked/,
  game.json and manifest.json must stay next to @EXE@.
- "error while loading shared libraries: libvulkan.so.1": install the Vulkan
  loader and a driver (Debian/Ubuntu: sudo apt install libvulkan1 mesa-vulkan-drivers).
- No suitable GPU: update the GPU driver. Without a GPU, Mesa's software
  driver works (slowly):
      VK_ICD_FILENAMES=/usr/share/vulkan/icd.d/lvp_icd.json ./@EXE@

Licences
--------
LICENSE and THIRD_PARTY_NOTICES.md (next to @EXE@) are the licences of the
quat-3d engine and of the third-party libraries this game ships with.
