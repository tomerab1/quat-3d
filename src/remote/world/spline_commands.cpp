// spline.* (ROADMAP G18.1): read, edit and query an entity's spline in world
// space. See docs/remote-control.md and docs/splines.md.

#include <cmath>
#include <string>

#include <nlohmann/json.hpp>

#include "engine/scene/component_json.hpp"
#include "engine/scene/components.hpp"
#include "engine/scene/scene.hpp"
#include "engine/scene/spline.hpp"
#include "engine/spline/frames.hpp"
#include "engine/spline/queries.hpp"
#include "internal.hpp"
#include "spline_internal.hpp"

namespace engine::remote::world {

namespace {

constexpr std::size_t k_max_reply_samples = 4096;

[[nodiscard]] Json sample_json(const spline::Frame& f, bool with_frame) {
    Json out = {{"position", vec3_json(f.position)},
                {"tangent", vec3_json(f.tangent)},
                {"width", f.width},
                {"height", f.height},
                {"tags", f.tags},
                {"distance", f.distance}};
    if (with_frame) {
        out["up"] = vec3_json(f.up);
        out["right"] = vec3_json(f.right);
    }
    return out;
}

RpcResult spline_get(Context& ctx, const Json& params) {
    core::JsonFields f(params, "");
    auto e = required_entity(f, ctx.scene->registry(), "entity");
    if (!e) return std::unexpected(e.error());
    if (auto ok = finish(f); !ok) return std::unexpected(ok.error());
    if (!ctx.scene->registry().all_of<scene::Spline>(*e)) {
        return rpc_fail(rpc_code::not_found, "entity: entity " + std::to_string(entt::to_integral(*e)) +
                                                 " has no spline (add one: spline.set {entity, points: [[x, y, z], ...]})");
    }
    // An invalid spline reads back with valid: false and the reason, so a
    // client can see the points it has to repair.
    return spline_summary(ctx, *e);
}

// {entity, ...spline fields}: merges into the entity's spline (adds one when
// absent). `points` replaces all points.
RpcResult spline_set(Context& ctx, const Json& params) {
    if (!params.is_object()) return bad_params("expected an object");
    if (!params.contains("entity")) return bad_params("entity: required");
    entt::registry& r = ctx.scene->registry();
    auto e = resolve_entity(r, params["entity"], "entity");
    if (!e) return std::unexpected(e.error());
    Json fields = params;
    fields.erase("entity");
    if (auto ok = scene::apply_component(r, *e, "spline", fields, true); !ok) return bad_params(ok.error().message);
    return spline_summary(ctx, *e);
}

// {entity, spacing=1, phase=0, fit_to_ends=false, frames?: up_locked |
// rotation_minimising, ground? (default: the spline's follow_ground)}.
RpcResult spline_sample(Context& ctx, const Json& params) {
    core::JsonFields f(params, "");
    auto e = required_entity(f, ctx.scene->registry(), "entity");
    if (!e) return std::unexpected(e.error());
    spline::SpacingOptions spacing;
    spacing.spacing = 1.0F;
    std::string frames;
    std::optional<bool> ground;
    bool ground_value = false;
    f.number("spacing", spacing.spacing, 0.01F, 1.0e5F)
        .number("phase", spacing.phase, -1.0e6F, 1.0e6F)
        .boolean("fit_to_ends", spacing.fit_to_ends)
        .string("frames", frames, 32);
    if (f.has("ground")) {
        f.boolean("ground", ground_value);
        ground = ground_value;
    }
    if (auto ok = finish(f); !ok) return std::unexpected(ok.error());
    spline::FrameMode mode = spline::FrameMode::up_locked;
    if (!frames.empty() && !spline::parse_frame_mode(frames, mode)) {
        return bad_params("frames: expected \"up_locked\" or \"rotation_minimising\"");
    }
    auto curve = spline_curve(ctx, *e);
    if (!curve) return std::unexpected(curve.error());
    const float needed = curve->length() / spacing.spacing;
    if (needed > static_cast<float>(k_max_reply_samples - 1)) {
        return bad_params("spacing: " + std::to_string(spacing.spacing) + " m gives more than " +
                          std::to_string(k_max_reply_samples) + " samples on this " +
                          std::to_string(curve->length()) + " m curve; use spacing >= " +
                          std::to_string(curve->length() / static_cast<float>(k_max_reply_samples - 1)));
    }
    std::vector<spline::CurveSample> samples = spline::sample_by_spacing(*curve, spacing);
    const scene::Spline& sp = ctx.scene->registry().get<scene::Spline>(*e);
    std::size_t missing = 0;
    if (ground.value_or(sp.follow_ground)) {
        const spline::GroundHeightFn fn = ground_fn(ctx);
        if (!fn) return unavailable("the ground (no terrain to drape on; pass ground: false)");
        missing = spline::drape(samples, fn, sp.ground_offset);
    }
    const std::vector<spline::Frame> fr = spline::frames(samples, mode);
    Json list = Json::array();
    for (const spline::Frame& x : fr) list.push_back(sample_json(x, !frames.empty()));
    return Json{{"entity", entity_json(*e)},
                {"length", curve->length()},
                {"samples", std::move(list)},
                {"missing_ground", missing}};
}

// {entity, point: [x, y, z], xz?=false}.
RpcResult spline_closest(Context& ctx, const Json& params) {
    core::JsonFields f(params, "");
    auto e = required_entity(f, ctx.scene->registry(), "entity");
    if (!e) return std::unexpected(e.error());
    glm::vec3 point(0.0F);
    bool xz = false;
    if (!f.has("point")) f.fail("point", "required: [x, y, z] in world space");
    f.vec3("point", point).boolean("xz", xz);
    if (auto ok = finish(f); !ok) return std::unexpected(ok.error());
    auto curve = spline_curve(ctx, *e);
    if (!curve) return std::unexpected(curve.error());
    const auto c = spline::closest_point(*curve, point, xz);
    if (!c) return rpc_fail(rpc_code::not_found, "the spline is empty");
    const spline::CurveSample s = curve->sample(c->param);
    return Json{{"param", c->param},
                {"distance", c->distance},
                {"position", vec3_json(c->position)},
                {"tangent", vec3_json(s.tangent)},
                {"width", s.width},
                {"gap", c->gap},
                {"length", curve->length()}};
}

// {entity, other? (entity; default: the spline's own self-crossings)}.
RpcResult spline_crossings(Context& ctx, const Json& params) {
    core::JsonFields f(params, "");
    entt::registry& r = ctx.scene->registry();
    auto e = required_entity(f, r, "entity");
    if (!e) return std::unexpected(e.error());
    const Json* other_ref = f.raw("other");
    if (auto ok = finish(f); !ok) return std::unexpected(ok.error());
    auto a = spline_curve(ctx, *e);
    if (!a) return std::unexpected(a.error());
    std::vector<spline::Crossing> hits;
    if (other_ref != nullptr && !other_ref->is_null()) {
        auto o = resolve_entity(r, *other_ref, "other");
        if (!o) return std::unexpected(o.error());
        auto b = spline_curve(ctx, *o, "other");
        if (!b) return std::unexpected(b.error());
        hits = spline::crossings_xz(*a, *b);
    } else {
        hits = spline::self_crossings_xz(*a);
    }
    Json list = Json::array();
    for (const spline::Crossing& h : hits) {
        list.push_back({{"param_a", h.param_a},
                        {"param_b", h.param_b},
                        {"position_a", vec3_json(h.position_a)},
                        {"position_b", vec3_json(h.position_b)}});
    }
    return Json{{"crossings", std::move(list)}};
}

// {entity, distance?=0, width_factor?=0, spacing?=0.25, trim?=true}.
RpcResult spline_offset(Context& ctx, const Json& params) {
    core::JsonFields f(params, "");
    auto e = required_entity(f, ctx.scene->registry(), "entity");
    if (!e) return std::unexpected(e.error());
    spline::OffsetOptions o;
    f.number("distance", o.distance, -1.0e4F, 1.0e4F)
        .number("width_factor", o.width_factor, -100.0F, 100.0F)
        .number("spacing", o.spacing, 0.01F, 1.0e4F)
        .boolean("trim", o.trim_loops);
    if (auto ok = finish(f); !ok) return std::unexpected(ok.error());
    if (o.distance == 0.0F && o.width_factor == 0.0F) {
        return bad_params("distance / width_factor: give a non-zero offset");
    }
    auto curve = spline_curve(ctx, *e);
    if (!curve) return std::unexpected(curve.error());
    if (curve->length() / o.spacing > static_cast<float>(k_max_reply_samples - 1)) {
        return bad_params("spacing: use spacing >= " +
                          std::to_string(curve->length() / static_cast<float>(k_max_reply_samples - 1)) +
                          " m for this curve (at most " + std::to_string(k_max_reply_samples) + " points)");
    }
    Json pts = Json::array();
    for (const glm::vec3& p : spline::offset_xz(*curve, o)) pts.push_back(vec3_json(p));
    return Json{{"closed", curve->closed()}, {"points", std::move(pts)}};
}

} // namespace

void register_spline_commands(RpcServer& server, const ContextPtr& ctx) {
    register_spline_network_commands(server, ctx);
    register_spline_fit_commands(server, ctx);
    server.add_method("spline.get",
                      "An entity's spline: {entity} -> {entity, spline (local points), length, segments, "
                      "bounds (world, swept by width / height)}.",
                      [ctx](const Json& p) { return spline_get(*ctx, p); });
    server.add_method("spline.set",
                      "Edit (or add) an entity's spline: {entity, kind?, points?, closed?, follow_ground?, "
                      "ground_offset?, network?, id?} -> as spline.get.",
                      [ctx](const Json& p) { return spline_set(*ctx, p); });
    server.add_method("spline.sample",
                      "Evenly spaced world-space samples: {entity, spacing?=1, phase?, fit_to_ends?, frames? "
                      "(up_locked | rotation_minimising), ground?} -> {length, samples, missing_ground}.",
                      [ctx](const Json& p) { return spline_sample(*ctx, p); });
    server.add_method("spline.closest",
                      "Closest point on a spline: {entity, point, xz?} -> {param, distance, position, tangent, "
                      "width, gap, length}.",
                      [ctx](const Json& p) { return spline_closest(*ctx, p); });
    server.add_method("spline.crossings",
                      "Crossings seen from above: {entity, other?} (self-crossings without other) -> "
                      "{crossings: [{param_a, param_b, position_a, position_b}]}.",
                      [ctx](const Json& p) { return spline_crossings(*ctx, p); });
    server.add_method("spline.offset",
                      "Offset curve in XZ: {entity, distance?, width_factor?, spacing?=0.25, trim?=true} -> "
                      "{closed, points}.",
                      [ctx](const Json& p) { return spline_offset(*ctx, p); });
}

} // namespace engine::remote::world
