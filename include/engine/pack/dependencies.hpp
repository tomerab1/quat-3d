#pragma once

// Which files a game ships (ROADMAP P4) — collected by following references,
// not by globbing the project:
//   game.json             the project itself, its start scene and icon
//   content roots         shipped whole (files only a running script finds)
//   scenes                start scene + scene files under content roots,
//                         decoded with the scene codec (scene_references.hpp):
//                         glTF models, audio clips, scripts
//   models                a .gltf's external buffers and images
//   scripts               require()d modules and string literals naming a
//                         script or a project file (lua_refs.hpp)
// Every path is confined to the project root. A scene reference that leaves
// the project or names a missing file is an error that says which scene and
// entity wrote it; a script literal that names nothing is only a warning
// (literals are not necessarily paths). Absolute paths inside the project
// (models spawned over MCP are saved that way) are recorded as rewrites for
// the staged copy of the scene.

#include <expected>
#include <filesystem>
#include <map>
#include <span>
#include <string>
#include <string_view>
#include <vector>

#include <nlohmann/json_fwd.hpp>

#include "engine/core/error.hpp"
#include "engine/project/game_project.hpp"
#include "engine/project/package_manifest.hpp"

namespace engine::pack {

inline constexpr std::string_view k_scripts_dir = "assets/scripts";

struct PackFile {
    std::string       path; // project-relative, '/' separators
    project::FileKind kind = project::FileKind::content;
    std::string       reason; // the first thing that needed it
    // model_data only: the models (project paths) referencing it.
    std::vector<std::string> owners;
};

struct PathRewrite {
    std::string from; // absolute path as written in the scene
    std::string to;   // project-relative replacement
};

struct GameFiles {
    std::vector<PackFile> files; // sorted by path, unique
    // Scene path -> absolute-to-relative rewrites its staged copy needs.
    std::map<std::string, std::vector<PathRewrite>> scene_rewrites;
    std::vector<std::string> warnings;
};

[[nodiscard]] std::expected<GameFiles, core::Error>
collect_game_files(const std::filesystem::path& project_root, const project::GameProject& project);

// Rewrites the path-valued fields of a scene document (mesh.gltf and the
// mesh/material cache keys derived from it, instanced_mesh.gltf,
// audio_source.clip): a value equal to `from`, or starting with `from` + '#'
// (cache keys), gets `to` instead. Returns how many strings changed.
std::size_t rewrite_scene_paths(nlohmann::json& scene, std::span<const PathRewrite> rewrites);

// What a file is, by extension (content roots, script literals).
[[nodiscard]] project::FileKind kind_for_path(std::string_view path);

} // namespace engine::pack
