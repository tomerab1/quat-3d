#pragma once

// Asset browser panel (Phase 9, Slice 9.5). Private editor header.

#include <string>

namespace engine::editor {

// Draws the "Assets" window: a filesystem tree of the project's asset
// directories (assets/, sample-assets/ under `project_root`). Double-clicking
// a glTF/GLB — or dropping one onto the Viewport panel — writes its absolute
// path into `instantiate_request`; the frame loop consumes it.
void draw_asset_browser(const std::string& project_root, std::string& instantiate_request);

// Payload type for glTF drag-and-drop (path string, used by the Viewport
// panel's drop target).
inline constexpr const char* asset_drag_payload = "QUAT_GLTF_PATH";

} // namespace engine::editor
