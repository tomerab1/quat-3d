#pragma once

// Confining untrusted project-relative paths (scene files, game.json, remote
// requests) to a root directory. A path from outside the process may name
// anything; these make sure it stays under the project it belongs to.

#include <expected>
#include <filesystem>
#include <string_view>

#include "engine/core/error.hpp"

namespace engine::core {

// Resolves a client-supplied project-relative path (UTF-8) under `root`:
// rejects empty, absolute and rooted paths and any `..` that escapes the root
// (lexically, and again after resolving symlinks of the existing prefix).
// Returns root / the normalised path.
[[nodiscard]] std::expected<std::filesystem::path, Error>
confine_to_root(const std::filesystem::path& root, std::string_view relative);

// The path of `path` relative to `root` when it lies inside it (both made
// absolute and normalised; lexical, no filesystem access), else empty.
[[nodiscard]] std::filesystem::path relative_inside(const std::filesystem::path& root,
                                                    const std::filesystem::path& path);

} // namespace engine::core
