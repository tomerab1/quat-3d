// Codec component `grass`: GPU-generated, wind-animated grass around the camera.
// Registered by the generated engine_components.cpp (ROADMAP S9); see
// include/engine/scene/component_registry.hpp and src/scene/codec/README.md.

#include <cmath>
#include <expected>
#include <string>
#include <string_view>
#include <utility>

#include "engine/core/json_fields.hpp"
#include "engine/scene/component_registry.hpp"
#include "engine/scene/components.hpp"
#include "scene/component_json_internal.hpp"

namespace engine::scene::codec {

namespace {

using namespace detail;
using core::EnumName;
using core::JsonFields;

[[nodiscard]] json encode_grass(const renderer::GrassSettings& g) {
    return {{"seed", g.seed},
            {"density", g.density},
            {"radius", g.radius},
            {"blade_height", range_json(g.height_range)},
            {"blade_width", range_json(g.width_range)},
            {"base_color", vec3_json(g.base_color)},
            {"tip_color", vec3_json(g.tip_color)},
            {"wind_strength", g.wind_strength},
            {"wind_direction", range_json(g.wind_direction)},
            {"wind_frequency", g.wind_frequency},
            {"slope", range_json(g.slope_deg)},
            {"height", range_json(g.height_m)},
            {"terrain_only", g.terrain_only},
            {"ground_y", g.ground_y},
            {"lod_start", g.lod_start},
            {"far_density", g.far_density}};
}

void decode_grass(JsonFields& f, renderer::GrassSettings& g) {
    f.integer("seed", g.seed, 0, 0xFFFFFFFFLL);
    f.number("density", g.density, 0.0F, 1000.0F).number("radius", g.radius, 0.0F, 500.0F);
    read_range(f, "blade_height", g.height_range, 0.01F, 5.0F);
    read_range(f, "blade_width", g.width_range, 0.001F, 0.5F);
    read_color3(f, "base_color", g.base_color);
    read_color3(f, "tip_color", g.tip_color);
    f.number("wind_strength", g.wind_strength, 0.0F, 4.0F);
    f.number("wind_frequency", g.wind_frequency, 0.0F, 10.0F);
    f.known({"wind_direction"});
    if (const json* w = f.raw("wind_direction"); w != nullptr && !w->is_null()) {
        glm::vec2 d{};
        if (w->is_array() && w->size() == 2 && JsonFields::read_float((*w)[0], d.x) &&
            JsonFields::read_float((*w)[1], d.y) && glm::length(d) > 1e-6F) {
            const float len = glm::length(d); // already-unit input round-trips exactly
            g.wind_direction = std::abs(len - 1.0F) < 1e-6F ? d : d / len;
        } else {
            f.fail("wind_direction", "expected a non-zero [x, z] direction");
        }
    }
    read_range(f, "slope", g.slope_deg, 0.0F, 90.0F);
    read_range(f, "height", g.height_m, -100000.0F, 100000.0F);
    f.boolean("terrain_only", g.terrain_only);
    f.number("ground_y", g.ground_y, -100000.0F, 100000.0F);
    f.number("lod_start", g.lod_start, 0.0F, 1.0F).number("far_density", g.far_density, 0.0F, 1.0F);
}

} // namespace

std::expected<void, core::Error> register_grass(ComponentRegistry& registry) {
    ComponentCodec codec{.name = "grass",
                         .summary = "GPU-generated, wind-animated grass around the camera",
                         .addable = true,
                         .writable = true,
                         .order = 1800};
    codec.encode = [](const entt::registry& r, entt::entity e, const EncodeOptions& /*options*/) {
        json out;
        const auto* g = &r.get<Grass>(e);
        out["grass"] = encode_grass(g->settings);
        return std::move(out["grass"]);
    };
    codec.apply = [](entt::registry& r, entt::entity e, const json& v, bool strict)
        -> std::expected<void, core::Error> {
        constexpr std::string_view name = "grass";
        return merge<Grass>(r, e, v, name, strict,
                            [](JsonFields& f, Grass& g) { decode_grass(f, g.settings); });
    };
    return registry.add(typed_codec<Grass>(std::move(codec)));
}

} // namespace engine::scene::codec
