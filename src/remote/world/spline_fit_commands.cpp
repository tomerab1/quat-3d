// spline.fit (ROADMAP G18.3): fit a drawn stroke (ground points) into a clean
// spline, optionally written to an entity. See docs/remote-control.md and
// docs/splines.md.

#include <string>

#include <nlohmann/json.hpp>

#include "engine/scene/component_json.hpp"
#include "engine/scene/scene.hpp"
#include "engine/scene/spline.hpp"
#include "engine/spline/stroke.hpp"
#include "internal.hpp"
#include "spline_internal.hpp"

namespace engine::remote::world {

namespace {

constexpr std::size_t k_max_stroke_points = 65536;

// [x, y, z] or {position: [x, y, z], straight?: bool}.
[[nodiscard]] bool read_stroke_point(const Json& v, spline::StrokePoint& out) {
    const Json* pos = &v;
    if (v.is_object()) {
        core::JsonFields f(v, "");
        pos = f.raw("position");
        f.boolean("straight", out.straight);
        if (pos == nullptr || !f.finish()) return false;
    }
    return pos->is_array() && pos->size() == 3 && core::JsonFields::read_vec3(*pos, out.position);
}

RpcResult spline_fit(Context& ctx, const Json& params) {
    core::JsonFields f(params, "");
    const Json* pts = f.raw("points");
    const Json* entity_ref = f.raw("entity");
    spline::StrokeOptions o;
    std::string output = "catmull_rom";
    bool ground = false;
    f.number("sample_spacing", o.sample_spacing, 0.01F, 100.0F)
        .number("jitter_tolerance", o.jitter_tolerance, 0.0F, 100.0F)
        .number("fit_tolerance", o.fit_tolerance, 0.001F, 100.0F)
        .number("corner_cos", o.corner_cos, -1.0F, 1.0F)
        .number("close_radius", o.close_radius, 0.0F, 1000.0F)
        .number("min_length", o.min_length, 0.0F, 1.0e6F)
        .number("width", o.width, 0.0F, spline::k_max_width)
        .string("output", output, 32)
        .boolean("ground", ground);
    if (pts == nullptr) f.fail("points", "required: the stroke as [[x, y, z], ...] (world metres)");
    if (auto ok = finish(f); !ok) return std::unexpected(ok.error());
    if (output == "cubic_bezier") {
        o.output = spline::StrokeOutput::cubic_bezier;
    } else if (output != "catmull_rom") {
        return bad_params("output: expected \"catmull_rom\" or \"cubic_bezier\"");
    }
    if (!pts->is_array() || pts->size() > k_max_stroke_points) {
        return bad_params("points: expected an array of at most " + std::to_string(k_max_stroke_points) +
                          " points");
    }
    std::vector<spline::StrokePoint> stroke;
    stroke.reserve(pts->size());
    for (std::size_t i = 0; i < pts->size(); ++i) {
        spline::StrokePoint p;
        if (!read_stroke_point((*pts)[i], p)) {
            return bad_params("points[" + std::to_string(i) +
                              "]: expected [x, y, z] or {position: [x, y, z], straight?: bool}");
        }
        stroke.push_back(p);
    }
    auto fit = spline::fit_stroke(stroke, o);
    if (!fit) return bad_params(fit.error().message);
    spline::Spline result = std::move(fit->spline);
    if (ground) {
        const spline::GroundHeightFn g = ground_fn(ctx);
        if (!g) return unavailable("the ground (no terrain to project on; pass ground: false)");
        (void)spline::project_points(result, g, 0.0F);
        result.follow_ground = true;
    }
    Json out = {{"closed", fit->closed}, {"pieces", fit->beziers.size()}};
    if (entity_ref != nullptr && !entity_ref->is_null()) {
        entt::registry& r = ctx.scene->registry();
        auto e = resolve_entity(r, *entity_ref, "entity");
        if (!e) return std::unexpected(e.error());
        // The stroke is in world space; the spline is entity-local.
        const glm::mat4 to_local = glm::inverse(world_matrix(r, *e));
        if (auto* existing = r.try_get<scene::Spline>(*e)) {
            result.network = existing->network;
            result.id = existing->id;
            result.ground_offset = existing->ground_offset;
        }
        r.emplace_or_replace<scene::Spline>(*e, spline::transformed(result, to_local));
        Json summary = spline_summary(ctx, *e);
        summary["closed_loop"] = fit->closed;
        summary["pieces"] = fit->beziers.size();
        return summary;
    }
    const std::string only[] = {"spline"};
    entt::registry scratch;
    const entt::entity tmp = scratch.create();
    scratch.emplace<scene::Spline>(tmp, result);
    scene::EncodeOptions opts;
    opts.only = only;
    out["spline"] = scene::encode_components(scratch, tmp, opts)["spline"];
    return out;
}

} // namespace

void register_spline_fit_commands(RpcServer& server, const ContextPtr& ctx) {
    server.add_method("spline.fit",
                      "Fit a drawn stroke into a clean spline: {points: [[x, y, z] | {position, straight}], "
                      "entity? (write the spline there), output? (catmull_rom | cubic_bezier), fit_tolerance?, "
                      "jitter_tolerance?, sample_spacing?, corner_cos?, close_radius?, min_length?, width?, "
                      "ground?} -> {spline, closed, pieces} (or spline.get's result with an entity).",
                      [ctx](const Json& p) { return spline_fit(*ctx, p); });
}

} // namespace engine::remote::world
