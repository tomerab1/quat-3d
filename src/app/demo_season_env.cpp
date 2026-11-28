// QUAT_SEASON: a season for the scene (ROADMAP R25.1) — see demo_scenes.hpp.

#include <cstdio>
#include <cstdlib>
#include <string>

#include <entt/entity/registry.hpp>
#include <nlohmann/json.hpp>

#include "demo_scenes.hpp"
#include "engine/scene/scene.hpp"
#include "engine/scene/season.hpp"
#include "engine/scene/season_api.hpp"
#include "engine/scene/world_clock.hpp"

namespace quat::app {

namespace {

// Seconds of simulated weather lived through before the first frame, so the
// surfaces show the season (snow lying in winter, wet ground after showers).
constexpr double k_env_spin_up_s = 300.0;
// A frozen clock (QUAT_TIME_SCALE=0, as screenshots use) would stop the
// surfaces too: the spin-up then runs the cozy 12-minute day.
constexpr double k_env_spin_up_time_scale = 1440.0 / 12.0;

} // namespace

void add_env_season(engine::scene::Scene& scene) {
    const char* which = std::getenv("QUAT_SEASON");
    if (which == nullptr) return;
    nlohmann::json spec = {{"mode", "manual"}, {"drive_clock_date", true}, {"spin_up_s", k_env_spin_up_s}};
    engine::scene::Season named{};
    if (engine::scene::parse_season(which, named)) {
        spec["season"] = which;
    } else {
        char*        end = nullptr;
        const double phase = std::strtod(which, &end);
        if (end == which || *end != '\0') {
            std::fprintf(stderr, "[season] QUAT_SEASON='%s': expected spring, summer, autumn, winter or a "
                                 "year phase in [0, 1)\n", which);
            return;
        }
        spec["year_phase"] = phase;
    }
    if (const char* seed = std::getenv("QUAT_SEASON_SEED"); seed != nullptr) {
        char*            end = nullptr;
        const long long  v = std::strtoll(seed, &end, 10);
        if (end == seed || *end != '\0' || v < 0 || v > 0xFFFFFFFFLL) {
            std::fprintf(stderr, "[season] QUAT_SEASON_SEED='%s': expected an integer in [0, 4294967295]\n", seed);
            return;
        }
        spec["weather_seed"] = v;
    }
    entt::registry&    r = scene.registry();
    const entt::entity ce = engine::scene::find_world_clock(r);
    const bool         frozen = ce != entt::null && r.get<engine::scene::WorldClock>(ce).time_scale == 0.0;
    if (frozen) r.get<engine::scene::WorldClock>(ce).time_scale = k_env_spin_up_time_scale;
    // The same validation as season.set.
    auto ok = engine::scene::set_season(scene, spec);
    if (frozen) r.get<engine::scene::WorldClock>(ce).time_scale = 0.0;
    if (!ok) {
        std::fprintf(stderr, "[season] QUAT_SEASON ignored: %s\n", ok.error().message.c_str());
        return;
    }
    const nlohmann::json report = engine::scene::season_report(scene.registry());
    std::fprintf(stderr, "[season] %s (phase %.3f), weather %s\n",
                 report["season"].get_ref<const std::string&>().c_str(), report["year_phase"].get<double>(),
                 report.contains("weather") ? report["weather"]["kind"].get_ref<const std::string&>().c_str()
                                            : "manual");
}

} // namespace quat::app
