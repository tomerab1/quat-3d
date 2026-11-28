#pragma once

// JSON codecs of the season (ROADMAP R25.1), shared by scene files (the
// `season` and `climate` components), scene.* remote commands and the
// season.* commands:
//
//   season:  {"enabled": true, "mode": "independent", "year_phase": 0.62,
//             "year": 2026, "phase_rate": 0.000416667, "drive_clock_date": true,
//             "weather_auto": true, "weather_seed": 7, "weather_clock_time": true}
//   climate: {"temperature_c": [12 numbers ...], "diurnal_range_c": [...],
//             "precipitation_chance": [...], "cloud_cover": [...],
//             "wind_speed": [...], "temperature_variability_c": 3,
//             "weather_period_days": 3, "transition": 0.3}
//
// Climate curves are 1..48 numbers, key i at year phase i / n (0 = the local
// spring equinox); each curve may have its own length.

#include <nlohmann/json_fwd.hpp>

namespace engine::core {
class JsonFields;
}
namespace engine::weather {
struct Climate;
}

namespace engine::scene {

struct SeasonState;

[[nodiscard]] nlohmann::json encode_season(const SeasonState& season);
// Reads the codec fields present in `fields` into `season` (merge: absent
// fields keep their values); problems are recorded on `fields`.
void decode_season(core::JsonFields& fields, SeasonState& season);

[[nodiscard]] nlohmann::json encode_climate(const weather::Climate& climate);
void decode_climate(core::JsonFields& fields, weather::Climate& climate);

} // namespace engine::scene
