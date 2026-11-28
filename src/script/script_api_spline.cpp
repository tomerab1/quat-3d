// The `spline` table scripts use (ROADMAP G18) — see docs/scripting.md
// ("Splines"). Queries run on the entity's spline in world space; edits go
// through the component codec (entity:set("spline", {...})).

#include <algorithm>
#include <cmath>
#include <string>

#include <entt/entity/registry.hpp>

#include <nlohmann/json.hpp>

#include "engine/scene/component_json.hpp"
#include "engine/scene/components.hpp"
#include "engine/scene/spline.hpp"
#include "engine/spline/frames.hpp"
#include "engine/spline/queries.hpp"
#include "engine/spline/stroke.hpp"
#include "lua_vm.hpp"
#include "script_api.hpp"

namespace engine::script::detail {

namespace {

constexpr std::size_t k_max_script_samples = 4096;
constexpr std::size_t k_max_stroke_points = 65536;

struct Resolved {
    spline::Curve             curve;
    const scene::Spline*      spline = nullptr;
    const scene::SplineCache* cache = nullptr; // fresh cache of the same curve, if any
};

[[nodiscard]] Result<Resolved> resolve(ApiContext& ctx, const ScriptEntity& e, std::string_view fn) {
    auto r = live_entity(ctx, e);
    if (!r.value) return std::unexpected(r.value.error());
    const entt::registry& reg = **r.value;
    const auto* s = reg.try_get<scene::Spline>(e.id);
    if (s == nullptr) return fail(std::string(fn) + ": entity " + describe_entity(reg, e.id) + " has no spline");
    auto c = spline::Curve::build(scene::world_spline(reg, e.id));
    if (!c) return fail(std::string(fn) + ": the spline is invalid: " + c.error().message);
    Resolved out{std::move(*c), s, reg.try_get<scene::SplineCache>(e.id)};
    if (out.cache != nullptr && (!out.cache->valid || out.cache->curve.length() != out.curve.length())) {
        out.cache = nullptr; // stale: the system rebuilds it next frame
    }
    return out;
}

// Ground height under a distance along the curve, interpolated from the
// cache's draped samples (scripts have no terrain sampler of their own).
void drape_from_cache(const Resolved& r, spline::CurveSample& s) {
    if (r.cache == nullptr || !r.spline->follow_ground || r.cache->samples.empty()) return;
    const auto& c = r.cache->samples;
    const auto it = std::lower_bound(c.begin(), c.end(), s.distance,
                                     [](const spline::CurveSample& a, float d) { return a.distance < d; });
    if (it == c.begin()) {
        s.position.y = c.front().position.y;
    } else if (it == c.end()) {
        s.position.y = c.back().position.y;
    } else {
        const auto& b = *it;
        const auto& a = *(it - 1);
        const float span = b.distance - a.distance;
        const float u = span > 0.0F ? (s.distance - a.distance) / span : 0.0F;
        s.position.y = a.position.y + (b.position.y - a.position.y) * u;
    }
}

[[nodiscard]] sol::table sample_table(lua_State* L, const spline::Frame& f, bool with_frame) {
    sol::table t(L, sol::create);
    t["position"] = f.position;
    t["tangent"] = f.tangent;
    t["width"] = f.width;
    t["height"] = f.height;
    t["tags"] = f.tags;
    t["distance"] = f.distance;
    if (with_frame) {
        t["up"] = f.up;
        t["right"] = f.right;
    }
    return t;
}

// spline.fit options, checked like the remote method's: known keys only,
// numbers in range, `output` one of two names.
[[nodiscard]] Result<void> read_stroke_options(const sol::table& opts, spline::StrokeOptions& o) {
    struct Range {
        const char* key;
        float*      out;
        float       lo;
        float       hi;
    };
    const Range numbers[] = {{"sample_spacing", &o.sample_spacing, 0.01F, 100.0F},
                             {"jitter_tolerance", &o.jitter_tolerance, 0.0F, 100.0F},
                             {"fit_tolerance", &o.fit_tolerance, 0.001F, 100.0F},
                             {"corner_cos", &o.corner_cos, -1.0F, 1.0F},
                             {"close_radius", &o.close_radius, 0.0F, 1000.0F},
                             {"min_length", &o.min_length, 0.0F, 1.0e6F},
                             {"width", &o.width, 0.0F, spline::k_max_width}};
    for (const auto& [key_obj, value] : opts) {
        if (key_obj.get_type() != sol::type::string) return fail("spline.fit: option keys must be names");
        const std::string key = key_obj.as<std::string>();
        if (key == "output") {
            const std::string out = value.is<std::string>() ? value.as<std::string>() : std::string();
            if (out == "cubic_bezier") {
                o.output = spline::StrokeOutput::cubic_bezier;
            } else if (out == "catmull_rom") {
                o.output = spline::StrokeOutput::catmull_rom;
            } else {
                return fail("spline.fit: output must be \"catmull_rom\" or \"cubic_bezier\"");
            }
            continue;
        }
        const Range* r = nullptr;
        for (const Range& n : numbers) {
            if (key == n.key) r = &n;
        }
        if (r == nullptr) {
            return fail("spline.fit: unknown option '" + key +
                        "' (known: sample_spacing, jitter_tolerance, fit_tolerance, corner_cos, close_radius, "
                        "min_length, width, output)");
        }
        const float v = value.get_type() == sol::type::number ? value.as<float>() : std::nanf("");
        if (!(v >= r->lo && v <= r->hi)) {
            return fail("spline.fit: " + key + " must be a number in [" + std::to_string(r->lo) + ", " +
                        std::to_string(r->hi) + "]");
        }
        *r->out = v;
    }
    return {};
}

} // namespace

void register_spline(LuaVm& vm, ApiContext& c) {
    lua_State* L = vm.state();
    ApiContext* ctx = &c;
    sol::table t(L, sol::create);

    t.set("length", bind(L, [ctx](const ScriptEntity& e) -> Result<float> {
              auto r = resolve(*ctx, e, "spline.length");
              if (!r.value) return std::unexpected(r.value.error());
              return r.value->curve.length();
          }));

    // spline.at(e, distance) -> {position, tangent, up, right, width, height, tags, distance}
    t.set("at", bind(L, [ctx](const ScriptEntity& e, float distance, sol::this_state s) -> Result<sol::object> {
              auto r = resolve(*ctx, e, "spline.at");
              if (!r.value) return std::unexpected(r.value.error());
              spline::CurveSample smp = r.value->curve.sample_at(distance);
              drape_from_cache(*r.value, smp);
              return sol::object(sample_table(s.lua_state(), spline::up_locked_frame(smp), true));
          }));

    // spline.sample(e, spacing, {phase, fit_to_ends, frames}) -> array of samples
    t.set("sample", bind(L, [ctx](const ScriptEntity& e, float spacing, sol::optional<sol::table> opts,
                                  sol::this_state s) -> Result<sol::object> {
              auto r = resolve(*ctx, e, "spline.sample");
              if (!r.value) return std::unexpected(r.value.error());
              const spline::Curve& curve = r.value->curve;
              if (!(spacing > 0.0F) || curve.length() / spacing > static_cast<float>(k_max_script_samples - 1)) {
                  return fail("spline.sample: spacing must give at most " + std::to_string(k_max_script_samples) +
                              " samples (use spacing >= " +
                              std::to_string(curve.length() / static_cast<float>(k_max_script_samples - 1)) + ")");
              }
              spline::SpacingOptions o;
              o.spacing = spacing;
              spline::FrameMode mode = spline::FrameMode::up_locked;
              bool with_frame = false;
              if (opts) {
                  o.phase = opts->get_or("phase", 0.0F);
                  o.fit_to_ends = opts->get_or("fit_to_ends", false);
                  const sol::optional<std::string> frames = opts->get<sol::optional<std::string>>("frames");
                  if (frames) {
                      if (!spline::parse_frame_mode(*frames, mode)) {
                          return fail("spline.sample: frames must be \"up_locked\" or \"rotation_minimising\"");
                      }
                      with_frame = true;
                  }
              }
              std::vector<spline::CurveSample> samples = spline::sample_by_spacing(curve, o);
              for (spline::CurveSample& smp : samples) drape_from_cache(*r.value, smp);
              const std::vector<spline::Frame> fr = spline::frames(samples, mode);
              sol::table out(s.lua_state(), sol::create);
              for (std::size_t i = 0; i < fr.size(); ++i) out[i + 1] = sample_table(s.lua_state(), fr[i], with_frame);
              return sol::object(out);
          }));

    // spline.closest(e, point, xz?) -> {position, distance, gap, param}
    t.set("closest", bind(L, [ctx](const ScriptEntity& e, const glm::vec3& point, sol::optional<bool> xz,
                                   sol::this_state s) -> Result<sol::object> {
              auto r = resolve(*ctx, e, "spline.closest");
              if (!r.value) return std::unexpected(r.value.error());
              const auto cp = spline::closest_point(r.value->curve, point, xz.value_or(false));
              if (!cp) return sol::make_object(s.lua_state(), sol::lua_nil);
              sol::table out(s.lua_state(), sol::create);
              out["position"] = cp->position;
              out["distance"] = cp->distance;
              out["gap"] = cp->gap;
              out["param"] = cp->param;
              return sol::object(out);
          }));

    // spline.fit(points, {fit_tolerance, jitter_tolerance, sample_spacing,
    // corner_cos, close_radius, width, output}) -> a `spline` component table
    // (for entity:set("spline", ...)), plus `closed`.
    t.set("fit", bind(L, [](const sol::table& points, sol::optional<sol::table> opts,
                            sol::this_state s) -> Result<sol::object> {
              std::vector<spline::StrokePoint> stroke;
              const std::size_t n = points.size();
              if (n > k_max_stroke_points) {
                  return fail("spline.fit: at most " + std::to_string(k_max_stroke_points) + " points");
              }
              for (std::size_t i = 1; i <= n; ++i) {
                  const sol::object item = points[i];
                  spline::StrokePoint p;
                  if (item.is<glm::vec3>()) {
                      p.position = item.as<glm::vec3>();
                  } else if (item.get_type() == sol::type::table) {
                      const sol::table pt = item.as<sol::table>();
                      const sol::object pos = pt["position"];
                      if (!pos.is<glm::vec3>()) {
                          return fail("spline.fit: points[" + std::to_string(i) + "].position must be a vec3");
                      }
                      p.position = pos.as<glm::vec3>();
                      p.straight = pt.get_or("straight", false);
                  } else {
                      return fail("spline.fit: points[" + std::to_string(i) +
                                  "] must be a vec3 or {position = vec3, straight = bool}");
                  }
                  stroke.push_back(p);
              }
              spline::StrokeOptions o;
              if (opts) {
                  if (auto ok = read_stroke_options(*opts, o); !ok.value) return std::unexpected(ok.value.error());
              }
              auto fit = spline::fit_stroke(stroke, o);
              if (!fit) return fail("spline.fit: " + fit.error().message);
              entt::registry scratch;
              const entt::entity tmp = scratch.create();
              scratch.emplace<scene::Spline>(tmp, fit->spline);
              const std::string only[] = {"spline"};
              scene::EncodeOptions eo;
              eo.only = only;
              nlohmann::json j = scene::encode_components(scratch, tmp, eo)["spline"];
              j["closed"] = fit->closed;
              return json_to_lua(s.lua_state(), j);
          }));

    vm.expose("spline", t);
}

} // namespace engine::script::detail
