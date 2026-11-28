#pragma once

// Environment variables decoded correctly on every platform. Windows keeps
// the environment in UTF-16 and std::getenv hands it out in the ANSI code
// page, so a path such as QUAT_SCREENSHOT under a non-ASCII (e.g. CJK)
// directory would be mangled; these read the wide environment there and are
// plain getenv elsewhere. Use env_path for anything that names a file.

#include <filesystem>
#include <optional>
#include <string>

namespace engine::core {

// The variable's value as UTF-8; nullopt when unset.
[[nodiscard]] std::optional<std::string> env_utf8(const char* name);

// The variable's value as a path; nullopt when unset or empty.
[[nodiscard]] std::optional<std::filesystem::path> env_path(const char* name);

} // namespace engine::core
