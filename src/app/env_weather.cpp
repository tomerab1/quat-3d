// QUAT_WEATHER=<preset>: start with a weather (R13.1) and, for the fog
// presets, the volumetric height fog (R13.3) — the CI screenshot rows of the
// R13.4 clouds use it (see demo_scenes.hpp).

#include <cstdio>
#include <cstdlib>
#include <string_view>

#include <entt/entity/registry.hpp>
#include <nlohmann/json.hpp>

#include "app/demo_scenes.hpp"
#include "engine/renderer/render_settings.hpp"
#include "engine/scene/component_json.hpp"
#include "engine/scene/components.hpp"
#include "engine/scene/scene.hpp"
#include "engine/weather/weather_state.hpp"

namespace quat::app {

namespace {

struct WeatherPreset {
    std::string_view name;
    float            cloud_cover;
    float            precipitation;
    float            wetness;
    float            cloud_type; // CloudSettings::type
    float            cirrus;     // CloudSettings::cirrus_coverage
    float            fog_density; // height fog extinction /m at the base (0 = none)
};

// Clear: a few fair-weather cumuli under thin cirrus. Cumulus: a field of
// cumulus humilis / mediocris. Overcast: a closed stratocumulus deck. Rain:
// a dark, raining deck with wet ground. Valley fog: a radiation fog filling
// the low ground (base at the camera's height, thinning with altitude).
constexpr WeatherPreset k_presets[] = {
    {"clear", 0.12F, 0.0F, 0.0F, 0.9F, 0.35F, 0.0F},
    {"cumulus", 0.45F, 0.0F, 0.0F, 0.95F, 0.15F, 0.0F},
    {"overcast", 0.97F, 0.0F, 0.0F, 0.5F, 0.0F, 0.0F},
    {"rain", 1.0F, 0.7F, 0.85F, 0.4F, 0.0F, 0.0F},
    {"valley_fog", 0.3F, 0.0F, 0.3F, 0.8F, 0.3F, 0.006F},
};

[[nodiscard]] float active_camera_height(const engine::scene::Scene& scene) {
    for (const auto [e, cam, t] : scene.registry()
                                      .view<const engine::scene::Camera,
                                            const engine::scene::Transform>()
                                      .each()) {
        if (cam.is_active) return t.world[3].y;
    }
    return 0.0F;
}

} // namespace

void add_env_weather(engine::scene::Scene& scene, engine::renderer::RenderSettings& settings) {
    const char* name = std::getenv("QUAT_WEATHER");
    if (name == nullptr) return;
    const WeatherPreset* preset = nullptr;
    for (const WeatherPreset& p : k_presets) {
        if (p.name == name) preset = &p;
    }
    if (preset == nullptr) {
        std::fprintf(stderr,
                     "[weather] QUAT_WEATHER=%s: unknown preset (clear, cumulus, overcast, rain, "
                     "valley_fog)\n",
                     name);
        return;
    }
    nlohmann::json w = {{"cloud_cover", preset->cloud_cover},
                        {"precipitation", preset->precipitation},
                        {"wetness", preset->wetness},
                        {"accumulate", false}};
    entt::entity e = engine::weather::find_weather(scene.registry());
    if (e == entt::null) e = scene.create_entity("weather");
    if (auto ok = engine::scene::apply_component(scene.registry(), e, "weather", w, true); !ok) {
        std::fprintf(stderr, "[weather] QUAT_WEATHER ignored: %s\n", ok.error().message.c_str());
        return;
    }
    settings.clouds.type = preset->cloud_type;
    settings.clouds.cirrus_coverage = preset->cirrus;
    if (preset->fog_density > 0.0F) {
        settings.fog.enabled = true;
        settings.fog.density = preset->fog_density;
        settings.fog.height_falloff = 0.012F;
        settings.fog.base_height_m = active_camera_height(scene) - 15.0F;
        settings.fog.max_distance_m = 1500.0F;
    }
    std::fprintf(stderr, "[weather] preset '%s' (cloud cover %.2f)\n", name, preset->cloud_cover);
}

} // namespace quat::app
