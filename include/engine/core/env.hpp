#pragma once

// Environment variables decoded correctly on every platform. Windows keeps
// the environment in UTF-16 and std::getenv hands it out in the ANSI code
// page, so a path such as QUAT_SCREENSHOT under a non-ASCII (e.g. CJK)
// directory would be mangled; these read the wide environment there and are
// plain getenv elsewhere. Use env_path for anything that names a file.

#include <filesystem>
#include <optional>
#include <string>
#include <string_view>

namespace engine::core {

// The variable's value as UTF-8; nullopt when unset.
[[nodiscard]] std::optional<std::string> env_utf8(const char* name);

// The variable's value as a path; nullopt when unset or empty.
[[nodiscard]] std::optional<std::filesystem::path> env_path(const char* name);

// A boolean switch's value: "1", "true", "yes", "on" are true; "0",
// "false", "no", "off" and the empty string are false (ASCII
// case-insensitive, surrounding blanks ignored); nullopt otherwise.
[[nodiscard]] std::optional<bool> parse_env_flag(std::string_view value);

// The boolean switch `name` (QUAT_NO_UI, QUAT_GAME_CAMERA, ...): `fallback`
// when unset; a value parse_env_flag does not recognise counts as on (the
// switch was set) and is reported on stderr once per call.
[[nodiscard]] bool env_flag(const char* name, bool fallback = false);

} // namespace engine::core
