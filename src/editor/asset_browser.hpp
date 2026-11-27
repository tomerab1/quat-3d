#pragma once

// Asset browser panel (Phase 9, Slice 9.5). Private editor header.

#include <string>

namespace engine::editor {

// Draws the "Assets" window: a filesystem tree of the project's asset
// directories (assets/, sample-assets/ under `project_root`). Double-clicking
// a glTF/GLB — or dropping one onto the Viewport panel — writes its absolute
// path into `instantiate_request`; the frame loop consumes it.
void draw_asset_browser(const std::string& project_root, std::string& instantiate_request);

// Payload type for asset drag-and-drop (UTF-8 path string): the Viewport
// panel places glTF files, asset fields accept matching files. Same value as
// ui::k_asset_payload.
inline constexpr const char* asset_drag_payload = "QUAT_ASSET_PATH";

} // namespace engine::editor
