// world.ground_height and terrain.status (issue #39), plus the terrain
// lookups the placement commands share.
//
// The runtime samples its tiles relative to the terrain's anchor; the anchor
// itself comes from the Terrain entity's transform as it is now, so a batch
// that moves the terrain and then queries heights sees the move.

#include <algorithm>
#include <string>
#include <vector>

#include <nlohmann/json.hpp>

#include "engine/scene/components.hpp"
#include "engine/scene/scene.hpp"
#include "internal.hpp"

namespace engine::remote::world {

std::optional<TerrainRef> find_terrain(const entt::registry& registry) {
    const auto view = registry.view<const scene::Terrain>();
    if (view.begin() == view.end()) return std::nullopt;
    const entt::entity e = *view.begin();
    const scene::Terrain& t = view.get<const scene::Terrain>(e);
    TerrainRef ref;
    ref.entity = e;
    ref.anchor = registry.all_of<scene::Transform>(e) ? glm::vec3(world_matrix(registry, e)[3])
                                                      : glm::vec3(0.0F);
    ref.streaming = t.streaming;
    ref.regenerating = t.regenerate;
    return ref;
}

std::optional<float> terrain_height_at(const Context& ctx, const TerrainRef& terrain, float x,
                                       float z) {
    if (!ctx.services.terrain_height || terrain.regenerating) return std::nullopt;
    const std::optional<float> h =
        ctx.services.terrain_height(x - terrain.anchor.x, z - terrain.anchor.z);
    if (!h) return std::nullopt;
    return terrain.anchor.y + *h;
}

namespace {

constexpr std::size_t k_max_height_points = 1024;

[[nodiscard]] std::unexpected<RpcError> no_terrain(std::string_view method) {
    return rpc_fail(rpc_code::not_found,
                    std::string(method) +
                        ": the scene has no terrain (no entity has a `terrain` component); add one "
                        "with scene.create, or place objects on other surfaces with "
                        "world.place_on_ground");
}

RpcResult ground_height(Context& ctx, const Json& params) {
    core::JsonFields f(params, "");
    const Json* points = f.raw("points");
    if (auto ok = finish(f); !ok) return std::unexpected(ok.error());
    if (points == nullptr || !points->is_array() || points->size() > k_max_height_points) {
        return bad_params("points: expected an array of at most " +
                          std::to_string(k_max_height_points) + " [x, z] pairs");
    }
    std::vector<glm::vec2> xz;
    xz.reserve(points->size());
    for (std::size_t i = 0; i < points->size(); ++i) {
        const Json& p = (*points)[i];
        glm::vec2 v{};
        if (!p.is_array() || p.size() != 2 || !core::JsonFields::read_float(p[0], v.x) ||
            !core::JsonFields::read_float(p[1], v.y)) {
            return bad_params("points[" + std::to_string(i) + "]: expected [x, z] (finite numbers)");
        }
        xz.push_back(v);
    }
    if (!ctx.services.terrain_height) return unavailable("terrain height sampling");
    const std::optional<TerrainRef> terrain = find_terrain(ctx.scene->registry());
    if (!terrain) return no_terrain("world.ground_height");

    Json heights = Json::array();
    for (const glm::vec2& p : xz) {
        const std::optional<float> h = terrain_height_at(ctx, *terrain, p.x, p.y);
        heights.push_back(h ? Json(*h) : Json(nullptr));
    }
    bool generating = terrain->regenerating;
    if (ctx.services.terrain_status) generating = generating || ctx.services.terrain_status().generating;
    return Json{{"heights", std::move(heights)}, {"generating", generating}};
}

RpcResult terrain_status(Context& ctx, const Json& params) {
    core::JsonFields f(params, "");
    if (auto ok = finish(f); !ok) return std::unexpected(ok.error());
    if (!ctx.services.terrain_status) return unavailable("terrain status");
    const entt::registry& r = ctx.scene->registry();
    const std::optional<TerrainRef> terrain = find_terrain(r);
    if (!terrain) return Json{{"present", false}};

    const scene::Terrain& t = r.get<const scene::Terrain>(terrain->entity);
    const TerrainStatus s = ctx.services.terrain_status();
    const auto side = static_cast<std::uint32_t>(2 * std::max(t.stream_radius, 0) + 1);
    // Before the runtime picks up a new terrain / shape, report what it will load.
    const std::uint32_t wanted = terrain->regenerating || s.tiles_wanted == 0
                                     ? (t.streaming ? side * side : 1U)
                                     : s.tiles_wanted;
    const std::uint32_t loaded = terrain->regenerating ? 0U : s.tiles_loaded;
    Json bounds = nullptr;
    if (s.has_bounds && !terrain->regenerating) {
        bounds = Json{{"min", vec3_json(terrain->anchor + s.bounds_min)},
                      {"max", vec3_json(terrain->anchor + s.bounds_max)}};
    }
    return Json{{"present", true},
                {"entity", entity_json(terrain->entity)},
                {"streaming", t.streaming},
                {"generating", terrain->regenerating || s.generating || loaded < wanted},
                {"tiles_loaded", loaded},
                {"tiles_wanted", wanted},
                {"anchor", vec3_json(terrain->anchor)},
                {"tile_size", t.params.tile_size_m},
                {"bounds", std::move(bounds)}};
}

} // namespace

void register_terrain_commands(RpcServer& server, const ContextPtr& ctx) {
    server.add_method("world.ground_height",
                      "Terrain height at world points: {points:[[x, z], ...]} (<= 1024) -> "
                      "{heights:[y | null], generating} (null where no tile is loaded yet); "
                      "not_found without terrain.",
                      [ctx](const Json& p) { return ground_height(*ctx, p); });
    server.add_method("terrain.status",
                      "Terrain generation state: {present, entity, streaming, generating, "
                      "tiles_loaded, tiles_wanted, anchor, tile_size, bounds:{min, max} | null}.",
                      [ctx](const Json& p) { return terrain_status(*ctx, p); });
}

} // namespace engine::remote::world
