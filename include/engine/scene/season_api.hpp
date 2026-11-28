#pragma once

// The season's control surface (ROADMAP R25.1), shared by the remote
// season.* commands (src/remote/world/season_commands.cpp) and the Lua
// `season` table (src/script/script_api_season.cpp), so both validate and
// report alike. The reply shape is docs/remote-control.md "Seasons".

#include <expected>

#include <entt/entity/fwd.hpp>
#include <nlohmann/json_fwd.hpp>

#include "engine/core/error.hpp"

namespace engine::scene {

class Scene;

inline constexpr double k_max_year_minutes = 1.0e6;
inline constexpr double k_max_day_minutes = 1.0e6;

// season.get: the season's codec fields, `in_scene`, `entity`, the dominant
// `season` and the four `weights`, `year_minutes` / `day_minutes` (the
// accelerated time scales), the day length, the climate now and, while
// weather_auto runs, the weather sequence's current period. Without a
// season: the default fields with `enabled` false and `in_scene` false.
[[nodiscard]] nlohmann::json season_report(const entt::registry& registry);

// season.set: the codec fields plus
//   season         "spring" | "summer" | "autumn" | "winter": the middle of it
//                  (exclusive with year_phase); in clock mode either moves the
//                  clock's date instead (keeping the time of day);
//   year_minutes   real minutes per year at simulation speed 1 (phase_rate);
//   day_minutes    real minutes per day: the world clock's time_scale;
//   real_calendar  true: clock mode, the world clock on today's date and time
//                  at time_scale 1 (created when the scene has none);
//   climate        an object merged into the season's Climate, or null for
//                  the default profile;
//   weather_preset a weather kind applied now in this season's climate
//                  (suspends weather_auto);
//   spin_up_s      simulate this many seconds up to the new state, so wetness
//                  and snow have accumulated (<= 3600).
// Decoded strictly (unknown fields are errors) and applied all or nothing.
// Creates the season when the scene has none.
[[nodiscard]] std::expected<void, core::Error> set_season(Scene& scene, const nlohmann::json& params);

} // namespace engine::scene
