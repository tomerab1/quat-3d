// environment.* (sun, clouds, sky source and HDRI).

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
#include "engine/scene/world_clock.hpp"
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

// R21: the sky source and the HDRI's numbers are the render settings'
// `environment` group; the HDRI file is scene data (services.set_hdri).
[[nodiscard]] const renderer::RenderSettingGroup& environment_group() {
    return *renderer::find_render_setting_group("environment");
}

[[nodiscard]] Json hdri_json(const Context& ctx) {
    Json out = Json::object();
    if (ctx.services.hdri_info) {
        const HdriInfo info = ctx.services.hdri_info();
        out["path"] = info.path.empty() ? Json(nullptr) : Json(info.path);
        out["loaded"] = info.loaded;
        out["width"] = info.width;
        out["height"] = info.height;
        out["sun_found"] = info.sun_found;
        if (!info.error.empty()) out["error"] = info.error;
    } else {
        out["path"] = nullptr;
        out["loaded"] = false;
    }
    if (ctx.services.render_settings != nullptr) {
        const renderer::EnvironmentSettings& e = ctx.services.render_settings->environment;
        out["intensity"] = e.hdri_intensity;
        out["rotation_deg"] = e.hdri_rotation_deg;
        out["sun"] = e.hdri_sun;
    }
    return out;
}

// The `environment` group fields an environment.set request carries:
// sky -> source, hdri.{intensity, rotation_deg, sun} -> hdri_*.
[[nodiscard]] Json environment_group_json(const Json* sky, const Json* hdri) {
    Json group = Json::object();
    if (sky != nullptr && !sky->is_null()) group["source"] = *sky;
    if (hdri != nullptr && hdri->is_object()) {
        for (const auto& [from, to] : {std::pair{"intensity", "hdri_intensity"},
                                       std::pair{"rotation_deg", "hdri_rotation_deg"},
                                       std::pair{"sun", "hdri_sun"}}) {
            if (const auto it = hdri->find(from); it != hdri->end()) group[to] = *it;
        }
    }
    return group;
}

RpcResult environment_get(Context& ctx, const Json& params) {
    core::JsonFields f(params, "");
    if (auto ok = finish(f); !ok) return std::unexpected(ok.error());
    const entt::registry& r = ctx.scene->registry();

    Json out = Json::object();
    if (const entt::entity sun = find_sun(r); sun != entt::null) {
        const auto& light = r.get<scene::DirectionalLight>(sun);
        const SunAngles a = angles_of(light.direction);
        // azimuth_deg here is the world-space angle of the light's travel
        // direction (the editor's convention; a scene without a world clock
        // has no compass). time.get reports the Sun's compass bearing.
        const entt::entity clock = scene::find_world_clock(r);
        const bool driven = clock != entt::null && r.get<scene::WorldClock>(clock).enabled &&
                            r.get<scene::WorldClock>(clock).drive_sun;
        out["sun"] = {{"azimuth_deg", a.azimuth_deg},
                      {"elevation_deg", a.elevation_deg},
                      {"intensity", light.intensity},
                      {"color", vec3_json(light.color)},
                      {"driven_by_clock", driven}};
    } else {
        out["sun"] = nullptr;
    }
    out["clouds"] = ctx.services.render_settings != nullptr
                        ? scene::encode_render_setting_group(*ctx.services.render_settings,
                                                             clouds_group())
                        : Json(nullptr);
    out["sky"] = ctx.services.render_settings != nullptr
                     ? Json(std::string(renderer::sky_source_names[static_cast<std::size_t>(
                           renderer::sky_source(ctx.services.render_settings->environment))]))
                     : Json(nullptr);
    out["hdri"] = hdri_json(ctx);
    return out;
}

RpcResult environment_set(Context& ctx, const Json& params) {
    entt::registry& r = ctx.scene->registry();
    core::JsonFields f(params, "");
    const Json* sun_json = f.raw("sun");
    const Json* clouds_in = f.raw("clouds");
    const Json* sky_in = f.raw("sky");
    const Json* hdri_in = f.raw("hdri");
    if (auto ok = finish(f); !ok) return std::unexpected(ok.error());

    // Decode both groups completely before changing anything.
    const entt::entity existing = find_sun(r);
    scene::DirectionalLight light =
        existing != entt::null ? r.get<scene::DirectionalLight>(existing) : scene::DirectionalLight{};
    bool set_sun = false;
    if (sun_json != nullptr && !sun_json->is_null()) {
        core::JsonFields s(*sun_json, "sun");
        // The world clock (R22.1) owns the sun's position and strength
        // while it drives the light; a write here would be undone next tick.
        if (const entt::entity c = scene::find_world_clock(r); c != entt::null) {
            const scene::WorldClock& clock = r.get<scene::WorldClock>(c);
            for (const char* key : {"azimuth_deg", "elevation_deg", "intensity"}) {
                if (clock.enabled && clock.drive_sun && s.has(key)) {
                    return bad_params(std::string("sun.") + key +
                                      ": the world clock drives the sun; change the time with "
                                      "time.set / time.skip (sun_intensity for the strength), or "
                                      "time.set {drive_sun:false} first");
                }
            }
        }
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

    // R21: sky source + HDRI. Every field is checked before anything changes;
    // the file loads last (a decode error then changes nothing either).
    std::optional<std::string> hdri_path; // set: load it ("" clears)
    if (hdri_in != nullptr && !hdri_in->is_null()) {
        if (!hdri_in->is_object()) return bad_params("hdri: expected an object");
        core::JsonFields h(*hdri_in, "hdri");
        // Checked below as the `environment` settings group's fields.
        for (const char* key : {"intensity", "rotation_deg", "sun"}) (void)h.raw(key);
        if (const Json* p = h.raw("path"); p != nullptr) {
            if (p->is_null() || (p->is_string() && p->get_ref<const std::string&>().empty())) {
                hdri_path = std::string();
            } else {
                std::string relative;
                auto confined = project_path(ctx, h, "path", relative);
                if (!confined) return std::unexpected(confined.error());
                hdri_path = relative;
            }
        }
        if (auto ok = finish(h); !ok) return std::unexpected(ok.error());
    }
    const Json env_fields = environment_group_json(sky_in, hdri_in);
    if (!env_fields.empty()) {
        if (ctx.services.render_settings == nullptr) return unavailable("sky control");
        if (!settings) settings = *ctx.services.render_settings;
        if (auto ok = scene::apply_render_setting_group(*settings, environment_group(), env_fields,
                                                        /*strict=*/true, "environment");
            !ok) {
            return bad_params(ok.error().message);
        }
    }
    if (hdri_path) {
        if (!ctx.services.set_hdri) return unavailable("HDRI loading");
        if (auto loaded = ctx.services.set_hdri(*hdri_path); !loaded) {
            return bad_params("hdri.path: " + loaded.error().message);
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
                      "light's travel direction d; intensity; linear color), cloud layer, sky "
                      "source and HDRI {path, loaded, width, height, sun_found, error?, "
                      "intensity, rotation_deg, sun}. sun/clouds are null when "
                      "absent/unavailable.",
                      [ctx](const Json& p) { return environment_get(*ctx, p); });
    server.add_method("environment.set",
                      "Set any of {sun:{azimuth_deg, elevation_deg, intensity, color}, "
                      "clouds:{enabled, coverage, density, bottom_km, thickness_km, size_km, "
                      "detail, wind_speed, wind_dir_deg, shadows}, sky: \"procedural\" | \"hdri\", "
                      "hdri:{path (project-relative .hdr; null clears), intensity, "
                      "rotation_deg, sun}}; returns environment.get.",
                      [ctx](const Json& p) { return environment_set(*ctx, p); });
}

} // namespace engine::remote::world
