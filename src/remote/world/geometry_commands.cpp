// geometry.* and mesh.dynamic.* — runtime procedural geometry (ROADMAP
// R23.2), through the JSON surface shared with the Lua `geometry` table
// (engine/scene/dynamic_mesh_api.hpp). docs/remote-control.md, "Runtime
// geometry".

#include <nlohmann/json.hpp>

#include "engine/scene/dynamic_mesh_api.hpp"
#include "internal.hpp"

namespace engine::remote::world {

namespace {

RpcResult wrap(std::expected<Json, core::Error> result) {
    if (!result) return bad_params(result.error().message);
    return std::move(*result);
}

// Methods that need the store report it unavailable without a renderer.
template <typename Fn>
RpcResult with_store(Context& ctx, Fn&& fn) {
    if (ctx.services.dynamic_meshes == nullptr) return unavailable("the dynamic mesh store");
    return wrap(fn(*ctx.services.dynamic_meshes));
}

} // namespace

void register_geometry_commands(RpcServer& server, const ContextPtr& ctx) {
    server.add_method("geometry.generate",
                      "Run a generator spec {type: roof | sweep | extrude | stone, ...} (docs/geometry.md). "
                      "With {id} the mesh is submitted to the dynamic mesh store (show it with a "
                      "dynamic_mesh component); without, only measured.",
                      [ctx](const Json& p) {
                          return wrap(scene::geometry_generate(ctx->services.dynamic_meshes, p));
                      });
    server.add_method("geometry.polygon",
                      "2D polygon operation {op: offset | union | intersect | subtract | xor | repair | "
                      "simplify, subject, clip?, delta?, join?, epsilon?, fill?} on [{outer, holes}] "
                      "polygons. Returns {polygons, area}.",
                      [](const Json& p) { return wrap(scene::geometry_polygon(p)); });
    server.add_method("mesh.dynamic.list", "Meshes in the dynamic mesh store and upload statistics.",
                      [ctx](const Json& p) {
                          return with_store(*ctx, [&](scene::DynamicMeshStore& s) { return scene::dynamic_mesh_list(s, p); });
                      });
    server.add_method("mesh.dynamic.get", "One dynamic mesh {id}: version, residency, size, bounds, hash.",
                      [ctx](const Json& p) {
                          return with_store(*ctx, [&](scene::DynamicMeshStore& s) { return scene::dynamic_mesh_get(s, p); });
                      });
    server.add_method("mesh.dynamic.set",
                      "Submit raw triangles {id, positions, indices, normals?, uvs?, crease_angle?}; "
                      "normals (by crease angle) and tangents are computed when absent.",
                      [ctx](const Json& p) {
                          return with_store(*ctx, [&](scene::DynamicMeshStore& s) { return scene::dynamic_mesh_set(s, p); });
                      });
    server.add_method("mesh.dynamic.update_vertices",
                      "Region update {id, first, positions, normals?}: replaces vertices first.. of the "
                      "latest version; only the changed bytes are uploaded.",
                      [ctx](const Json& p) {
                          return with_store(*ctx,
                                            [&](scene::DynamicMeshStore& s) { return scene::dynamic_mesh_update(s, p); });
                      });
    server.add_method("mesh.dynamic.remove",
                      "Remove {id} from the store; entities showing it stop drawing it.",
                      [ctx](const Json& p) {
                          return with_store(*ctx,
                                            [&](scene::DynamicMeshStore& s) { return scene::dynamic_mesh_remove(s, p); });
                      });
}

} // namespace engine::remote::world
