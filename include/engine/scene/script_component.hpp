#pragma once

// Script component rules shared by the codec, the script runtime, the editor
// and the remote API (G7): which script paths and property names are valid,
// and how property values are encoded.
//
// Script paths are relative to the project's assets/scripts/ directory, use
// '/' separators, end in ".lua" and consist of [A-Za-z0-9_.-] segments with
// no "." / ".." segment — a whitelist, so a path from a scene file or a remote
// client can never leave the scripts directory.

#include <cstddef>
#include <expected>
#include <string>
#include <string_view>

#include <nlohmann/json_fwd.hpp>

#include "engine/core/error.hpp"

namespace engine::core {
class JsonFields;
}

namespace engine::scene {

struct Script;
struct ScriptProperty;

inline constexpr std::string_view k_scripts_dir = "assets/scripts";
inline constexpr std::size_t      k_max_script_path = 200;
inline constexpr std::size_t      k_max_script_properties = 64;
inline constexpr std::size_t      k_max_property_name = 64;
inline constexpr std::size_t      k_max_property_text = 4096;

// A valid script path (see the header comment); the error says what is wrong.
[[nodiscard]] std::expected<void, core::Error> validate_script_path(std::string_view path);

// A Lua identifier of at most k_max_property_name bytes, not "entity" (the
// instance's own handle).
[[nodiscard]] std::expected<void, core::Error> validate_property_name(std::string_view name);

// Property value as JSON: number, boolean, string or [x, y, z].
[[nodiscard]] nlohmann::json property_value_json(const ScriptProperty& property);

// Decodes a property value from JSON (type inferred as above) into `out`
// (name untouched). The error names what was expected.
[[nodiscard]] std::expected<void, core::Error> decode_property_value(const nlohmann::json& value,
                                                                     ScriptProperty& out);

// The codec's "script" object: {path, enabled, properties:{name: value}}.
[[nodiscard]] nlohmann::json encode_script(const Script& script);

// Merge-decodes the codec's "script" fields: `properties` merges per name
// (null removes one); any accepted properties change bumps `revision`.
void decode_script_fields(core::JsonFields& fields, Script& script);

} // namespace engine::scene
