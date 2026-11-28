#pragma once

// Script files (G7): compile checks and access to the scripts directory
// (assets/scripts/ under the project root), shared by the script runtime and
// the remote script.* commands.

#include <cstddef>
#include <expected>
#include <filesystem>
#include <string>
#include <string_view>
#include <vector>

#include "engine/core/error.hpp"

namespace engine::script {

inline constexpr std::size_t k_max_script_bytes = 256 * 1024;
inline constexpr std::size_t k_max_listed_scripts = 2000;

// A Lua compile error: `line` is 1-based (0 when the message names none),
// `message` is Lua's text including "<chunk>:<line>:".
struct SyntaxError {
    int         line = 0;
    std::string message;
};

// Compiles `source` (text only, never bytecode) without running it.
// `chunk_name` is the path shown in messages ("coin.lua").
[[nodiscard]] std::expected<void, SyntaxError> check_syntax(std::string_view source,
                                                            std::string_view chunk_name);

// The absolute file of script `relative` (validated with
// scene::validate_script_path) under `scripts_root`. Refused when an
// existing prefix resolves outside the root (a symlink).
[[nodiscard]] std::expected<std::filesystem::path, core::Error>
script_file(const std::filesystem::path& scripts_root, std::string_view relative);

// Reads a script (at most k_max_script_bytes).
[[nodiscard]] std::expected<std::string, core::Error> read_script(const std::filesystem::path& file);

// Every *.lua under `scripts_root` (recursive), as sorted relative paths,
// at most k_max_listed_scripts; files whose path is not a valid script path
// are skipped.
[[nodiscard]] std::vector<std::string> list_scripts(const std::filesystem::path& scripts_root);

} // namespace engine::script
