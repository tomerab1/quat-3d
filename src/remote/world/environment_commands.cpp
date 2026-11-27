// environment.* (sun + clouds) and world.ground_height.

#include <cmath>
#include <optional>
#include <string>

#include <glm/gtc/constants.hpp>
#include <nlohmann/json.hpp>

#include "engine/scene/component_json.hpp"
#include "engine/scene/components.hpp"
#include "engine/scene/scene.hpp"
#include "internal.hpp"

namespace engine::remote::world {

namespace {

constexpr std::size_t k_max_height_points = 1024;

// Sun convention (shared with the editor's Renderer panel): the light's travel
// direction d gives azimuth = atan2(d.z, d.x), elevation = asin(-d.y).
struct SunAngles {
    float azimuth_deg = 0.0F;
    float elevation_deg = 45.0F;
};

[[nodiscard]] SunAngles angles_of(const glm::vec3& direction) {
    const glm::vec3 d = glm::normalize(direction);
    return {glm::degrees(std::atan2(d.z, d.x)),
            glm::degrees(std::asin(glm::clamp(-d.y, -1.0F, 1.0F)))};
}

[[nodiscard]] glm::vec3 direction_of(const SunAngles& a) {
    const float az = glm::radians(a.azimuth_deg);
    const float el = glm::radians(a.elevation_deg);
    return {std::cos(el) * std::cos(az), -std::sin(el), std::cos(el) * std::sin(az)};
}

[[nodiscard]] entt::entity find_sun(const entt::registry& r) {
    for (const entt::entity e : r.view<const scene::DirectionalLight>()) return e;
    return entt::null;
}

[[nodiscard]] Json clouds_json(const renderer::CloudSettings& c) {
    return Json{{"enabled", c.enabled},         {"coverage", c.coverage},
                {"density", c.density},         {"bottom_km", c.bottom_km},
                {"thickness_km", c.thickness_km}, {"size_km", c.size_km},
                {"detail", c.detail},           {"wind_speed", c.wind_speed},
                {"wind_dir_deg", c.wind_dir_deg}};
}

RpcResult environment_get(Context& ctx, const Json& params) {
    core::JsonFields f(params, "");
    if (auto ok = finish(f); !ok) return std::unexpected(ok.error());
    const entt::registry& r = ctx.scene->registry();

    Json out = Json::object();
    if (const entt::entity sun = find_sun(r); sun != entt::null) {
        const auto& light = r.get<scene::DirectionalLight>(sun);
        const SunAngles a = angles_of(light.direction);
        out["sun"] = {{"azimuth_deg", a.azimuth_deg},
                      {"elevation_deg", a.elevation_deg},
                      {"intensity", light.intensity},
                      {"color", vec3_json(light.color)}};
    } else {
        out["sun"] = nullptr;
    }
    out["clouds"] = ctx.services.clouds_get ? clouds_json(ctx.services.clouds_get()) : Json(nullptr);
    return out;
}

RpcResult environment_set(Context& ctx, const Json& params) {
    entt::registry& r = ctx.scene->registry();
    core::JsonFields f(params, "");
    const Json* sun_json = f.raw("sun");
    const Json* clouds_in = f.raw("clouds");
    if (auto ok = finish(f); !ok) return std::unexpected(ok.error());

    // Decode both groups completely before changing anything.
    const entt::entity existing = find_sun(r);
    scene::DirectionalLight light =
        existing != entt::null ? r.get<scene::DirectionalLight>(existing) : scene::DirectionalLight{};
    bool set_sun = false;
    if (sun_json != nullptr && !sun_json->is_null()) {
        core::JsonFields s(*sun_json, "sun");
        SunAngles a = existing != entt::null ? angles_of(light.direction) : SunAngles{};
        s.number("azimuth_deg", a.azimuth_deg, -360.0F, 360.0F);
        s.number("elevation_deg", a.elevation_deg, -90.0F, 90.0F);
        s.number("intensity", light.intensity, 0.0F, 1.0e5F);
        if (const Json* c = s.raw("color")) {
            glm::vec4 rgba(light.color, 1.0F);
            if (c->is_array()) {
                // Light colours may exceed 1 (HDR tint); arrays go through color3.
                const Json holder{{"color", *c}}; // JsonFields keeps a reference
                core::JsonFields wrap(holder, "sun");
                wrap.color3("color", light.color);
                if (auto ok = wrap.finish(); !ok) s.fail("color", "expected [r, g, b] >= 0 or \"#rrggbb\"");
            } else if (auto ok = scene::decode_color(*c, rgba); ok) {
                light.color = glm::vec3(rgba);
            } else {
                s.fail("color", ok.error().message);
            }
        }
        if (auto ok = finish(s); !ok) return std::unexpected(ok.error());
        light.direction = direction_of(a);
        set_sun = true;
    }

    std::optional<renderer::CloudSettings> clouds;
    if (clouds_in != nullptr && !clouds_in->is_null()) {
        if (!ctx.services.clouds_get || !ctx.services.clouds_set) return unavailable("cloud control");
        renderer::CloudSettings c = ctx.services.clouds_get();
        core::JsonFields s(*clouds_in, "clouds");
        s.boolean("enabled", c.enabled);
        s.number("coverage", c.coverage, 0.0F, 1.0F).number("density", c.density, 0.0F, 1000.0F);
        s.number("bottom_km", c.bottom_km, 0.0F, 30.0F).number("thickness_km", c.thickness_km, 0.1F, 30.0F);
        s.number("size_km", c.size_km, 0.5F, 200.0F).number("detail", c.detail, 0.0F, 1.0F);
        s.number("wind_speed", c.wind_speed, 0.0F, 500.0F);
        s.number("wind_dir_deg", c.wind_dir_deg, -360.0F, 360.0F);
        if (auto ok = finish(s); !ok) return std::unexpected(ok.error());
        clouds = c;
    }

    if (set_sun) {
        entt::entity sun = existing;
        if (sun == entt::null) sun = ctx.scene->create_entity("sun");
        r.emplace_or_replace<scene::DirectionalLight>(sun, light);
    }
    if (clouds) ctx.services.clouds_set(*clouds);
    return environment_get(ctx, Json::object());
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
    if (!ctx.services.ground_height) return unavailable("terrain height sampling");
    Json heights = Json::array();
    for (const glm::vec2& p : xz) {
        const std::optional<float> h = ctx.services.ground_height(p.x, p.y);
        heights.push_back(h ? Json(*h) : Json(nullptr));
    }
    return Json{{"heights", std::move(heights)}};
}

} // namespace

void register_environment_commands(RpcServer& server, const ContextPtr& ctx) {
    server.add_method("environment.get",
                      "Sun (azimuth_deg = atan2(d.z, d.x), elevation_deg = asin(-d.y) of the "
                      "light's travel direction d; intensity; linear color) and cloud layer. "
                      "sun/clouds are null when absent/unavailable.",
                      [ctx](const Json& p) { return environment_get(*ctx, p); });
    server.add_method("environment.set",
                      "Set any of {sun:{azimuth_deg, elevation_deg, intensity, color}, "
                      "clouds:{enabled, coverage, density, bottom_km, thickness_km, size_km, "
                      "detail, wind_speed, wind_dir_deg}}; returns environment.get.",
                      [ctx](const Json& p) { return environment_set(*ctx, p); });
    server.add_method("world.ground_height",
                      "Terrain height at world points: {points:[[x, z], ...]} (<= 1024) -> "
                      "{heights:[y | null]} (null where no terrain covers the point).",
                      [ctx](const Json& p) { return ground_height(*ctx, p); });
}

} // namespace engine::remote::world
