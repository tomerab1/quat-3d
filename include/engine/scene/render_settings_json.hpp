#pragma once

// JSON codec of renderer::RenderSettings (ROADMAP A8), driven entirely by the
// descriptor table in engine/renderer/render_settings.hpp. Shared by the
// remote API (render.settings.*, environment.* clouds) and scene files (the
// optional "render_settings" block).
//
// Shape: {"<group>": {"<field>": value, ...}, ...} — booleans as true/false,
// integers as integers, numbers as numbers.

#include <expected>
#include <string_view>

#include <nlohmann/json_fwd.hpp>

#include "engine/core/error.hpp"

namespace engine::renderer {
struct RenderSettings;
struct RenderSettingGroup;
} // namespace engine::renderer

namespace engine::scene {

// Version of the scene-file block (written as "version" inside it).
inline constexpr int k_render_settings_version = 1;

// Every group, or (non-empty `group`) only {"<group>": {...}}.
[[nodiscard]] nlohmann::json encode_render_settings(const renderer::RenderSettings& settings,
                                                    std::string_view group = {});

// One group's fields as {"<field>": value, ...}.
[[nodiscard]] nlohmann::json encode_render_setting_group(const renderer::RenderSettings& settings,
                                                         const renderer::RenderSettingGroup& group);

// Merges `patch` ({"<group>": {"<field>": value}}) into `settings`; absent
// groups and fields keep their values, and nothing changes on error.
//   strict (remote requests): unknown groups / fields, wrong types and values
//     outside a field's range are errors naming the field and what it takes.
//   lenient (scene files): unknown groups / fields are ignored and numbers
//     outside the range are clamped (a file written by a newer build still
//     loads); wrong types are still errors.
[[nodiscard]] std::expected<void, core::Error>
apply_render_settings_json(renderer::RenderSettings& settings, const nlohmann::json& patch,
                           bool strict);

// As apply_render_settings_json, for one group's fields ({"<field>": value}).
// `context` prefixes error messages (e.g. "clouds").
[[nodiscard]] std::expected<void, core::Error>
apply_render_setting_group(renderer::RenderSettings& settings,
                           const renderer::RenderSettingGroup& group, const nlohmann::json& fields,
                           bool strict, std::string_view context);

// The descriptor table: {"groups": [{name, label, hint, fields: [{name, label,
// type: "boolean"|"number"|"integer", min, max, default, unit, hint}]}]}.
[[nodiscard]] nlohmann::json render_settings_schema();

} // namespace engine::scene
