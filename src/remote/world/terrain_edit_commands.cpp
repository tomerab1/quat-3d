// terrain.edit.* (ROADMAP G19.1): the terrain's ordered edit stamps, in world
// space, through scene::TerrainEdits. See docs/remote-control.md and
// docs/terrain-edits.md. Edits apply on the next frame; terrain.status and
// world.ground_height see them from then on.

#include <string>
#include <vector>

#include <nlohmann/json.hpp>

#include "engine/scene/scene.hpp"
#include "engine/scene/terrain_edits.hpp"
#include "internal.hpp"

namespace engine::remote::world {

namespace {

constexpr std::size_t k_max_remove_ids = 4096;

[[nodiscard]] std::unexpected<RpcError> edit_failed(const core::Error& e) {
    const bool no_terrain = e.message.rfind("the scene has no terrain", 0) == 0;
    return rpc_fail(no_terrain ? rpc_code::not_found : rpc_code::invalid_params, e.message);
}

[[nodiscard]] Json result(scene::TerrainEdits& edits, Json out) {
    out["revision"] = edits.revision();
    out["count"] = edits.list().size();
    return out;
}

// {shape, points, radius, falloff, op, strength, height, pivot, normal,
//  amount, iterations, channel, value, before?} -> {id, revision, count}
RpcResult edit_add(Context& ctx, const Json& params) {
    if (params.is_object() && params.contains("id")) {
        return bad_params("id: assigned by terrain.edit.add (use terrain.edit.update to change "
                          "a stamp)");
    }
    core::JsonFields f(params, "");
    std::optional<std::uint64_t> before;
    if (f.has("before")) {
        std::uint64_t b = 0;
        f.integer("before", b, 1, std::numeric_limits<std::int64_t>::max());
        before = b;
    }
    terrain::TerrainStamp stamp;
    // Optional fields default as on the stamp (circle, raise, ...).
    scene::decode_stamp(f, stamp);
    if (auto ok = finish(f); !ok) return std::unexpected(ok.error());
    scene::TerrainEdits edits(ctx.scene->registry());
    auto id = edits.add(stamp, before);
    if (!id) return edit_failed(id.error());
    return result(edits, Json{{"id", *id}});
}

// {id, <stamp fields to change>} -> {id, revision, count}
RpcResult edit_update(Context& ctx, const Json& params) {
    core::JsonFields f(params, "");
    std::uint64_t id = 0;
    f.integer("id", id, 1, std::numeric_limits<std::int64_t>::max());
    if (!f.has("id")) return bad_params("id: required (from terrain.edit.add / list)");
    scene::TerrainEdits edits(ctx.scene->registry());
    std::optional<terrain::TerrainStamp> stamp = edits.get(id);
    if (!f.ok()) return std::unexpected(finish(f).error());
    if (!stamp) {
        if (!scene::find_terrain_anchor(ctx.scene->registry())) {
            return rpc_fail(rpc_code::not_found, "the scene has no terrain");
        }
        return rpc_fail(rpc_code::not_found, "id: no stamp with id " + std::to_string(id) +
                                                 " (terrain.edit.list shows them)");
    }
    scene::decode_stamp(f, *stamp); // merges the given fields
    if (auto ok = finish(f); !ok) return std::unexpected(ok.error());
    if (auto ok = edits.update(id, *stamp); !ok) return edit_failed(ok.error());
    return result(edits, Json{{"id", id}});
}

// {id} | {ids:[...]} -> {removed:[...], missing:[...], revision, count}
RpcResult edit_remove(Context& ctx, const Json& params) {
    core::JsonFields f(params, "");
    std::vector<std::uint64_t> ids;
    if (f.has("id")) {
        std::uint64_t id = 0;
        f.integer("id", id, 1, std::numeric_limits<std::int64_t>::max());
        ids.push_back(id);
    }
    if (const Json* list = f.raw("ids")) {
        if (!list->is_array() || list->size() > k_max_remove_ids) {
            return bad_params("ids: expected an array of at most " +
                              std::to_string(k_max_remove_ids) + " stamp ids");
        }
        for (std::size_t i = 0; i < list->size(); ++i) {
            std::int64_t v = 0;
            if (!core::JsonFields::read_int((*list)[i], v) || v < 1) {
                return bad_params("ids[" + std::to_string(i) + "]: expected a stamp id (>= 1)");
            }
            ids.push_back(static_cast<std::uint64_t>(v));
        }
    }
    if (auto ok = finish(f); !ok) return std::unexpected(ok.error());
    if (ids.empty()) return bad_params("expected id or ids");
    if (!scene::find_terrain_anchor(ctx.scene->registry())) {
        return rpc_fail(rpc_code::not_found, "the scene has no terrain");
    }
    scene::TerrainEdits edits(ctx.scene->registry());
    Json removed = Json::array();
    Json missing = Json::array();
    for (const std::uint64_t id : ids) {
        if (edits.remove(id)) {
            removed.push_back(id);
        } else {
            missing.push_back(id);
        }
    }
    return result(edits, Json{{"removed", std::move(removed)}, {"missing", std::move(missing)}});
}

// {} -> {stamps:[...], revision, count}
RpcResult edit_list(Context& ctx, const Json& params) {
    core::JsonFields f(params, "");
    if (auto ok = finish(f); !ok) return std::unexpected(ok.error());
    if (!scene::find_terrain_anchor(ctx.scene->registry())) {
        return rpc_fail(rpc_code::not_found, "the scene has no terrain");
    }
    scene::TerrainEdits edits(ctx.scene->registry());
    Json stamps = Json::array();
    for (const terrain::TerrainStamp& s : edits.list()) stamps.push_back(scene::encode_stamp(s));
    return result(edits, Json{{"stamps", std::move(stamps)}});
}

// {} -> {removed: n, revision, count}
RpcResult edit_clear(Context& ctx, const Json& params) {
    core::JsonFields f(params, "");
    if (auto ok = finish(f); !ok) return std::unexpected(ok.error());
    if (!scene::find_terrain_anchor(ctx.scene->registry())) {
        return rpc_fail(rpc_code::not_found, "the scene has no terrain");
    }
    scene::TerrainEdits edits(ctx.scene->registry());
    const std::size_t n = edits.clear();
    return result(edits, Json{{"removed", n}});
}

} // namespace

void register_terrain_edit_commands(RpcServer& server, const ContextPtr& ctx) {
    server.add_method("terrain.edit.add",
                      "Add a terrain edit stamp (world space): {shape: circle|capsule|polygon, "
                      "points:[[x, z]...], radius, falloff, op: flatten|raise|lower|smooth|paint, "
                      "strength, height, pivot, normal, amount, iterations, channel, value, "
                      "before?} -> {id, revision, count}.",
                      [ctx](const Json& p) { return edit_add(*ctx, p); });
    server.add_method("terrain.edit.update",
                      "Change fields of a stamp in place: {id, <stamp fields>} -> {id, revision, "
                      "count}.",
                      [ctx](const Json& p) { return edit_update(*ctx, p); });
    server.add_method("terrain.edit.remove",
                      "Remove stamps: {id} | {ids:[...]} -> {removed, missing, revision, count}.",
                      [ctx](const Json& p) { return edit_remove(*ctx, p); });
    server.add_method("terrain.edit.list",
                      "The terrain's edit stamps in application order (world space): {} -> "
                      "{stamps, revision, count}.",
                      [ctx](const Json& p) { return edit_list(*ctx, p); });
    server.add_method("terrain.edit.clear", "Remove every stamp: {} -> {removed, revision, count}.",
                      [ctx](const Json& p) { return edit_clear(*ctx, p); });
}

} // namespace engine::remote::world
