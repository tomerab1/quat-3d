#pragma once

// The `spline` component of the component JSON codec (component_json.cpp;
// ROADMAP G18.1).

#include <nlohmann/json.hpp>

#include "engine/core/json_fields.hpp"
#include "engine/scene/spline.hpp"

namespace engine::scene::detail {

[[nodiscard]] nlohmann::json encode_spline(const Spline& spline);
// Merges fields into `spline`; `points` (when present) replaces every point.
// The result is validated whole (spline::validate), so a field that makes
// the curve impossible (a closed polyline of 2 points) is an error.
void decode_spline(core::JsonFields& fields, Spline& spline);

// One point: [x, y, z] or {position, width?, height?, tags?}.
[[nodiscard]] nlohmann::json encode_spline_point(const SplinePoint& point);
[[nodiscard]] bool decode_spline_point(const nlohmann::json& v, std::string context, bool strict,
                                       SplinePoint& out, std::string& error);

} // namespace engine::scene::detail
