#pragma once

// JSON control surface of runtime geometry (ROADMAP R23.2), shared by the
// remote methods (geometry.*, mesh.dynamic.*), the MCP tools built on them and
// the Lua `geometry` table, so all three accept and return the same objects.
// Parameters are untrusted: read through core::JsonFields, sizes bounded.
// docs/remote-control.md ("Runtime geometry") documents every call.

#include <expected>

#include <nlohmann/json.hpp>

#include "engine/core/error.hpp"
#include "engine/scene/dynamic_mesh.hpp"

namespace engine::scene {

// {id, version, resident_version, vertices, triangles, bytes, bounds, hash}.
[[nodiscard]] nlohmann::json dynamic_mesh_info_json(const DynamicMeshInfo& info);

// geometry.generate {spec, id?}: runs a generator spec (geometry_spec.hpp).
// With `id` the mesh is submitted to `store` (returns its info); without, it
// is only measured ({vertices, triangles, bounds, hash}).
[[nodiscard]] std::expected<nlohmann::json, core::Error> geometry_generate(DynamicMeshStore* store,
                                                                         const nlohmann::json& params);

// geometry.polygon {op, subject, clip?, delta?, join?, miter_limit?,
// arc_tolerance?, epsilon?, fill?}: 2D polygon operations. op is offset,
// union, intersect, subtract, xor, repair or simplify; polygons are
// [{outer: [[x, y], ...], holes: [[[x, y], ...], ...]}]. Returns
// {polygons, area}.
[[nodiscard]] std::expected<nlohmann::json, core::Error> geometry_polygon(const nlohmann::json& params);

// mesh.dynamic.set {id, positions, indices, normals?, uvs?, crease_angle?,
// submeshes?}: submits raw triangles. Missing normals are computed (crease
// angle, default 40 degrees); tangents always are (MikkTSpace).
[[nodiscard]] std::expected<nlohmann::json, core::Error> dynamic_mesh_set(DynamicMeshStore& store,
                                                                         const nlohmann::json& params);

// mesh.dynamic.update_vertices {id, first, positions, normals?}: a region
// update of the latest version (same topology).
[[nodiscard]] std::expected<nlohmann::json, core::Error> dynamic_mesh_update(DynamicMeshStore& store,
                                                                            const nlohmann::json& params);

// mesh.dynamic.list {} -> {meshes: [info...], stats}; mesh.dynamic.get {id};
// mesh.dynamic.remove {id} -> {removed}.
[[nodiscard]] std::expected<nlohmann::json, core::Error> dynamic_mesh_list(const DynamicMeshStore& store,
                                                                          const nlohmann::json& params);
[[nodiscard]] std::expected<nlohmann::json, core::Error> dynamic_mesh_get(const DynamicMeshStore& store,
                                                                         const nlohmann::json& params);
[[nodiscard]] std::expected<nlohmann::json, core::Error> dynamic_mesh_remove(DynamicMeshStore& store,
                                                                            const nlohmann::json& params);

} // namespace engine::scene
