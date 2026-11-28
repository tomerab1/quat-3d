// The `spline` table scripts use (ROADMAP G18) — see docs/scripting.md
// ("Splines"). Queries run on the entity's spline in world space; edits go
// through the component codec (entity:set("spline", {...})).

#include <algorithm>
#include <string>

#include <entt/entity/registry.hpp>

#include "engine/scene/components.hpp"
#include "engine/scene/spline.hpp"
#include "engine/spline/frames.hpp"
#include "engine/spline/queries.hpp"
#include "lua_vm.hpp"
#include "script_api.hpp"

namespace engine::script::detail {

namespace {

constexpr std::size_t k_max_script_samples = 4096;

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

    vm.expose("spline", t);
}

} // namespace engine::script::detail
