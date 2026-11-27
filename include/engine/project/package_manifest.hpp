#pragma once

// Package manifest (ROADMAP P4): `manifest.json` at the data root of a game
// packaged by quat-pack. It lists every shipped data file (path, size,
// SHA-256, kind, cooked) plus the game's identity, the package config, the
// target and the build id. The runtime player reads the config from it (a
// shipping package refuses remote control); tools can verify a package with
// it. Written deterministically: no timestamps, files sorted by path.

#include <cstdint>
#include <expected>
#include <string>
#include <string_view>
#include <vector>

#include <nlohmann/json_fwd.hpp>

#include "engine/core/error.hpp"

namespace engine::project {

inline constexpr int              k_package_manifest_format = 1;
inline constexpr std::string_view k_package_manifest_file = "manifest.json";

// development: the player honours QUAT_REMOTE (MCP can drive the packaged
// game); shipping: remote control stays off whatever the environment says.
enum class PackageConfig : std::uint8_t { development, shipping };

[[nodiscard]] std::string_view to_string(PackageConfig config);
[[nodiscard]] std::expected<PackageConfig, core::Error> parse_package_config(std::string_view s);

// What a shipped file is (for tools and the build log).
enum class FileKind : std::uint8_t { project, scene, model, model_data, audio, script, icon,
                                     shader, content };

[[nodiscard]] std::string_view to_string(FileKind kind);

struct ManifestFile {
    std::string   path;   // data-root-relative, '/' separators
    std::uint64_t bytes = 0;
    std::string   sha256; // lower-case hex
    FileKind      kind = FileKind::content;
    bool          cooked = false; // written by quat-cook rather than copied
};

struct PackageManifest {
    int           format = k_package_manifest_format;
    std::string   game_id;
    std::string   game_name;
    std::string   game_version;
    PackageConfig config = PackageConfig::shipping;
    std::string   target;   // e.g. "linux-x64"
    std::string   build_id; // empty when none was given
    std::string   engine;   // engine version that packaged it
    std::vector<ManifestFile> files; // sorted by path
};

[[nodiscard]] nlohmann::json manifest_json(const PackageManifest& manifest);

// Strict decode; a newer format is an error that says so.
[[nodiscard]] std::expected<PackageManifest, core::Error>
parse_manifest(const nlohmann::json& doc);

} // namespace engine::project
