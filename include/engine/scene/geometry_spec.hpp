#pragma once

// Declarative geometry generator specs (ROADMAP R23.2): one JSON object that
// names an engine_geometry generator and its inputs. Scene files carry them
// on DynamicMesh components (so a saved scene regenerates its meshes), and
// the remote `geometry.*` methods, the MCP tools and Lua take the same
// objects. The schema is in docs/geometry.md ("Generator specs").
//
//   {"type": "roof",    "polygon": [[x, y], ...], "holes": [[[x, y], ...]],
//                       "weights": [...], "base_height", "height_scale",
//                       "max_height", "uv_scale"}
//   {"type": "sweep",   "path": [[x, y, z], ...], "profile": [[x, y], ...],
//                       "closed_path", "closed_profile", "caps", "miter_limit",
//                       "uv_scale", "crease_angle"}
//   {"type": "extrude", "polygon", "holes", "height", "base_height", "taper",
//                       "caps", "uv_scale"}
//   {"type": "stone",   "seed", "size": [x, y, z], "subdivisions", "roundness",
//                       "noise", "chips", "chip_depth", "uv_scale"}
//
// Specs are untrusted input: every field is read through core::JsonFields,
// point counts and the output size are bounded, and the error names the
// offending field.

#include <cstddef>
#include <expected>

#include <nlohmann/json.hpp>

#include "engine/core/error.hpp"
#include "engine/geometry/mesh_data.hpp"

namespace engine::scene {

// Output bound of one spec, in vertices (a sweep's path x profile count is
// checked before anything is generated).
inline constexpr std::size_t k_geometry_spec_max_vertices = 1U << 20;

// Runs the generator `spec` names. `context` prefixes error messages.
[[nodiscard]] std::expected<geometry::MeshData, core::Error>
generate_from_spec(const nlohmann::json& spec, std::string_view context = "geometry");

} // namespace engine::scene
