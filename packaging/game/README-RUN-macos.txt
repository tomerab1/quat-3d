@NAME@ - macOS (Apple Silicon)

Run
---
The app is ad-hoc signed but not notarized yet, so the first launch of a
downloaded copy is blocked by Gatekeeper. Allow it once, either way:

  - Double-click @APP@.app, dismiss the dialog, then open
    System Settings -> Privacy & Security and click "Open Anyway" next to
    @APP@, and confirm (on macOS 14, right-click the app -> Open -> Open
    also works).
  - Or, in Terminal: xattr -dr com.apple.quarantine "@APP@.app"

After that it opens with a double-click. Made with the quat-3d engine.

Requirements
------------
- An Apple Silicon Mac (arm64), macOS @MINOS@ or later. Vulkan runs on Metal
  through MoltenVK, which ships inside the app - nothing to install.

Troubleshooting
---------------
- Nothing happens on launch: run "@APP@.app/Contents/MacOS/@EXE@" from
  Terminal to read its log.
