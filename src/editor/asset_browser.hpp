#pragma once

// Asset browser panel (Phase 9 slice 9.5, redesigned in ROADMAP E8.3):
// breadcrumbs, a folder tree, grid and list views with type icons, a
// recursive search, drag-and-drop of any file (glTF models onto the viewport
// place them; asset fields accept matching files), double-click to open a
// folder or place a model, and "Show in file manager". Private editor header.

#include <chrono>
#include <cstdint>
#include <filesystem>
#include <string>
#include <string_view>
#include <vector>

namespace engine::editor {

// Payload type for asset drag-and-drop (UTF-8 path string): the Viewport
// panel places glTF files, asset fields accept matching files. Same value as
// ui::k_asset_payload.
inline constexpr const char* asset_drag_payload = "QUAT_ASSET_PATH";

// What a file is, from its extension.
enum class AssetKind : std::uint8_t { folder, model, texture, shader, scene, audio, font, text, other };
struct AssetTypeInfo {
    AssetKind   kind;
    const char* icon;  // icon:: constant
    const char* label; // "Model", "Texture", ...
};
[[nodiscard]] AssetTypeInfo asset_type(const std::filesystem::path& path, bool is_directory);

struct AssetBrowserState {
    std::filesystem::path dir = "assets"; // current folder, project-relative ("" = top)
    std::string           filter;
    bool                  grid = true;
    float                 tile = 84.0F; // grid tile width (logical px)
    // Directory listing cache (refreshed on navigation, search or every few
    // seconds), so the panel does not hit the filesystem every frame.
    struct Entry {
        std::filesystem::path path; // absolute
        std::string           name; // UTF-8 file name (or project-relative path in search)
        bool                  is_dir = false;
        std::uintmax_t        size = 0;
    };
    std::vector<Entry>                    entries;
    std::filesystem::path                 cached_dir;
    std::string                           cached_filter;
    std::chrono::steady_clock::time_point cached_at{};
    bool                                  dirty = true;
};

// Draws the "Assets" window (`window_name`, closable through `open`).
// Double-clicking a glTF/GLB writes its absolute path into
// `instantiate_request`; the frame loop consumes it.
void draw_asset_browser(const std::string& project_root, std::string& instantiate_request,
                        AssetBrowserState& state, const char* window_name, bool* open);

} // namespace engine::editor
