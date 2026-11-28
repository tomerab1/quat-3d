// Shared helpers of the spline.* world commands (ROADMAP G18).

#include <string>

#include "engine/scene/component_json.hpp"
#include "engine/scene/scene.hpp"
#include "engine/scene/spline.hpp"
#include "spline_internal.hpp"

namespace engine::remote::world {

std::expected<spline::Curve, RpcError> spline_curve(const Context& ctx, entt::entity e, std::string_view field) {
    const entt::registry& r = ctx.scene->registry();
    if (!r.all_of<scene::Spline>(e)) {
        return rpc_fail(rpc_code::not_found,
                        std::string(field) + ": entity " + std::to_string(entt::to_integral(e)) +
                            " has no spline (add one: spline.set {entity, points: [[x, y, z], ...]})");
    }
    auto c = spline::Curve::build(spline::transformed(r.get<scene::Spline>(e), world_matrix(r, e)));
    if (!c) return bad_params(std::string(field) + ": its spline is invalid: " + c.error().message);
    return std::move(*c);
}

Json spline_summary(const Context& ctx, entt::entity e) {
    const entt::registry& r = ctx.scene->registry();
    const std::string only[] = {"spline"};
    scene::EncodeOptions opts;
    opts.only = only;
    Json out = scene::encode_components(r, e, opts);
    out["entity"] = entity_json(e);
    auto c = spline::Curve::build(spline::transformed(r.get<scene::Spline>(e), world_matrix(r, e)));
    if (!c) {
        out["valid"] = false;
        out["error"] = c.error().message;
        return out;
    }
    const spline::Bounds b = c->swept_bounds();
    out["valid"] = true;
    out["length"] = c->length();
    out["segments"] = c->segment_count();
    out["bounds"] = {{"min", vec3_json(b.min)}, {"max", vec3_json(b.max)}};
    return out;
}

spline::GroundHeightFn ground_fn(const Context& ctx) {
    const std::optional<TerrainRef> terrain = find_terrain(ctx.scene->registry());
    if (!terrain || !ctx.services.terrain_height) return {};
    return [&ctx, t = *terrain](float x, float z) { return terrain_height_at(ctx, t, x, z); };
}

} // namespace engine::remote::world
