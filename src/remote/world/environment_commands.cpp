// environment.* (sun + clouds).

#include <cmath>
#include <optional>
#include <string>

#include <glm/gtc/constants.hpp>
#include <nlohmann/json.hpp>

#include "engine/renderer/render_settings.hpp"
#include "engine/scene/component_json.hpp"
#include "engine/scene/components.hpp"
#include "engine/scene/render_settings_json.hpp"
#include "engine/scene/scene.hpp"
#include "internal.hpp"

namespace engine::remote::world {

namespace {

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

// The cloud layer is the `clouds` group of the render settings: same codec,
// same ranges as render.settings.*.
[[nodiscard]] const renderer::RenderSettingGroup& clouds_group() {
    return *renderer::find_render_setting_group("clouds");
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
    out["clouds"] = ctx.services.render_settings != nullptr
                        ? scene::encode_render_setting_group(*ctx.services.render_settings,
                                                             clouds_group())
                        : Json(nullptr);
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

    std::optional<renderer::RenderSettings> settings;
    if (clouds_in != nullptr && !clouds_in->is_null()) {
        if (ctx.services.render_settings == nullptr) return unavailable("cloud control");
        settings = *ctx.services.render_settings;
        if (auto ok = scene::apply_render_setting_group(*settings, clouds_group(), *clouds_in,
                                                        /*strict=*/true, "clouds");
            !ok) {
            return bad_params(ok.error().message);
        }
    }

    if (set_sun) {
        entt::entity sun = existing;
        if (sun == entt::null) sun = ctx.scene->create_entity("sun");
        r.emplace_or_replace<scene::DirectionalLight>(sun, light);
    }
    if (settings) *ctx.services.render_settings = *settings;
    return environment_get(ctx, Json::object());
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
                      "detail, wind_speed, wind_dir_deg, shadows}}; returns environment.get.",
                      [ctx](const Json& p) { return environment_set(*ctx, p); });
}

} // namespace engine::remote::world
