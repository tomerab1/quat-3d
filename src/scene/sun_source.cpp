// Sun ownership — see include/engine/scene/sun_source.hpp.

#include "engine/scene/sun_source.hpp"

#include <entt/entity/registry.hpp>
#include <glm/glm.hpp>

#include "engine/renderer/hdri_sun.hpp"
#include "engine/renderer/reflection_settings.hpp"
#include "engine/scene/world_clock.hpp"

namespace engine::scene {

SunSource sun_source(const entt::registry& registry,
                     const renderer::EnvironmentSettings& environment, bool hdri_loaded) {
    if (hdri_loaded && environment.hdri_sun &&
        renderer::sky_source(environment) == renderer::SkySource::hdri) {
        return SunSource::hdri;
    }
    if (const entt::entity e = find_world_clock(registry); e != entt::null) {
        const WorldClock& clock = registry.get<WorldClock>(e);
        if (clock.enabled && clock.drive_sun) return SunSource::clock;
    }
    return SunSource::manual;
}

SceneSun resolve_sun(const entt::registry& registry,
                     const renderer::EnvironmentSettings& environment,
                     const renderer::HdriSun* hdri) {
    SceneSun out;
    out.source = sun_source(registry, environment, hdri != nullptr);
    if (out.source == SunSource::hdri) {
        const renderer::EnvironmentSettings env = environment.sanitized();
        const renderer::HdriSunLight sun =
            renderer::hdri_sun_light(*hdri, env.hdri_rotation_deg, env.hdri_intensity);
        // A panorama without a sun (overcast, interior) puts the light out.
        out.light = DirectionalLight{-sun.direction_to_sun, sun.color,
                                     sun.found ? sun.intensity : 0.0F};
        return out;
    }
    // The renderer lights with the first DirectionalLight.
    for (const entt::entity e : registry.view<const DirectionalLight>()) {
        out.light = registry.get<const DirectionalLight>(e);
        break;
    }
    return out;
}

} // namespace engine::scene
