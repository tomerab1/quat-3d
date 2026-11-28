#pragma once

// The external files a scene file names (ROADMAP P4: the packager ships only
// what a game uses). Entities are decoded exactly as load_scene decodes them
// — the same entity reader rules and the component codec, into a scratch
// registry, no GPU — and the typed components are asked for their files:
//   mesh.gltf                  the glTF a mesh was instanced from
//   instanced_mesh.gltf        the model of an instanced mesh
//   audio_source.clip          a sound clip
//   script.path                a Lua script (G7; relative to assets/scripts/)
// Paths are reported as written (project-relative or absolute); resolving
// and confining them is the caller's job.

#include <cstdint>
#include <expected>
#include <filesystem>
#include <string>
#include <string_view>
#include <vector>

#include <nlohmann/json_fwd.hpp>

#include "engine/core/error.hpp"

namespace engine::scene {

enum class SceneRefKind : std::uint8_t { model, audio, script };

[[nodiscard]] std::string_view to_string(SceneRefKind kind);

struct SceneReference {
    SceneRefKind kind = SceneRefKind::model;
    std::string  path;  // as written in the file
    std::string  where; // e.g. "entities[3] 'Player' audio_source.clip"
};

// References of a parsed scene document, in file order (duplicates kept: the
// caller dedupes, and `where` says who referenced what).
[[nodiscard]] std::expected<std::vector<SceneReference>, core::Error>
scene_references(const nlohmann::json& root);

// Reads, parses and scans a scene file. The error names the file.
[[nodiscard]] std::expected<std::vector<SceneReference>, core::Error>
scene_file_references(const std::filesystem::path& path);

// True when `doc` has the shape of a scene file (an object with an
// "entities" array) — how content roots tell scenes from other JSON.
[[nodiscard]] bool is_scene_document(const nlohmann::json& doc);

} // namespace engine::scene
