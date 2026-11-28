// Codec component `scatter`: Seeded procedural placement of the entity's instanced mesh on the terrain.
// Registered by the generated engine_components.cpp (ROADMAP S9); see
// include/engine/scene/component_registry.hpp and src/scene/codec/README.md.

#include <expected>
#include <string>
#include <string_view>
#include <utility>

#include "engine/core/json_fields.hpp"
#include "engine/scene/component_registry.hpp"
#include "engine/scene/components.hpp"
#include "engine/scene/scatter.hpp"
#include "scene/component_json_internal.hpp"

namespace engine::scene::codec {

namespace {

using namespace detail;
using core::EnumName;
using core::JsonFields;

constexpr EnumName<ScatterArea> k_scatter_areas[] = {{"rect", ScatterArea::rect},
                                                     {"circle", ScatterArea::circle}};
constexpr EnumName<ScatterPattern> k_scatter_patterns[] = {
    {"poisson", ScatterPattern::poisson}, {"jittered_grid", ScatterPattern::jittered_grid}};

} // namespace

std::expected<void, core::Error> register_scatter(ComponentRegistry& registry) {
    ComponentCodec codec{.name = "scatter",
                         .summary = "Seeded procedural placement of the entity's instanced mesh on the terrain",
                         .addable = true,
                         .writable = true,
                         .order = 500};
    codec.encode = [](const entt::registry& r, entt::entity e, const EncodeOptions& /*options*/) {
        json out;
        const auto* sc = &r.get<Scatter>(e);
        out["scatter"] = {{"seed", sc->seed},
                          {"area", enum_name(k_scatter_areas, sc->area)},
                          {"size", json::array({sc->size.x, sc->size.y})},
                          {"radius", sc->radius},
                          {"density", sc->density},
                          {"min_spacing", sc->min_spacing},
                          {"pattern", enum_name(k_scatter_patterns, sc->pattern)},
                          {"scale", json::array({sc->scale_range.x, sc->scale_range.y})},
                          {"random_yaw", sc->random_yaw},
                          {"align_to_normal", sc->align_to_normal},
                          {"base_offset", sc->base_offset},
                          {"mirror", sc->mirror},
                          {"slope", json::array({sc->slope_deg.x, sc->slope_deg.y})},
                          {"height", json::array({sc->height_m.x, sc->height_m.y})},
                          {"max_instances", sc->max_instances}};
        return std::move(out["scatter"]);
    };
    codec.apply = [](entt::registry& r, entt::entity e, const json& v, bool strict)
        -> std::expected<void, core::Error> {
        constexpr std::string_view name = "scatter";
        return merge<Scatter>(r, e, v, name, strict, [](JsonFields& f, Scatter& s) {
            f.integer("seed", s.seed, 0, 0xFFFFFFFFLL);
            f.enumeration("area", s.area, k_scatter_areas);
            f.known({"size"});
            if (const json* sz = f.raw("size"); sz != nullptr && !sz->is_null()) {
                glm::vec2 size{};
                if (sz->is_array() && sz->size() == 2 && JsonFields::read_float((*sz)[0], size.x) &&
                    JsonFields::read_float((*sz)[1], size.y) && size.x >= 0.0F && size.y >= 0.0F &&
                    size.x <= 20000.0F && size.y <= 20000.0F) {
                    s.size = size;
                } else {
                    f.fail("size", "expected [x, z] in metres, each in [0, 20000]");
                }
            }
            f.number("radius", s.radius, 0.0F, 10000.0F);
            f.number("density", s.density, 0.0F, 100.0F);
            f.number("min_spacing", s.min_spacing, 0.0F, 1000.0F);
            f.enumeration("pattern", s.pattern, k_scatter_patterns);
            read_range(f, "scale", s.scale_range, 0.001F, 1000.0F);
            f.boolean("random_yaw", s.random_yaw);
            f.number("align_to_normal", s.align_to_normal, 0.0F, 1.0F);
            f.number("base_offset", s.base_offset, -1000.0F, 1000.0F);
            f.number("mirror", s.mirror, 0.0F, 1.0F);
            read_range(f, "slope", s.slope_deg, 0.0F, 90.0F);
            read_range(f, "height", s.height_m, -100000.0F, 100000.0F);
            f.integer("max_instances", s.max_instances, 0, static_cast<std::int64_t>(k_max_instances));
        });
    };
    codec.remove = [](entt::registry& r, entt::entity e) {
        return r.remove<Scatter>(e) > 0; // keeps the placements (baked)
    };
    return registry.add(typed_codec<Scatter>(std::move(codec)));
}

} // namespace engine::scene::codec
