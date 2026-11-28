#pragma once

// SeasonState — where the scene is in its year (ROADMAP R25.1; design:
// docs/design/cozy-builder.md §4.14).
//
// One per scene (a component on one entity, like WorldClock; the codec
// refuses a second). `year_phase` is in [0, 1): 0 is the local spring
// equinox, 0.25 the summer solstice, 0.5 the autumn equinox, 0.75 the winter
// solstice — the astronomical seasons, so "local" flips them in the southern
// hemisphere. season_system() runs in Scene::tick after the world clock:
//
//   clock        the phase is the apparent solar ecliptic longitude / 360
//                (R22.1's Sun), shifted by half a year south of the equator,
//                so equinoxes and solstices fall on their real dates for the
//                clock's latitude. "Follow the real calendar" is this mode
//                with the clock on today's date at time_scale 1.
//   independent  the phase advances by phase_rate years per simulated second
//                (a 40-minute year: 1 / 2400), decoupled from the day cycle;
//                with drive_clock_date the season writes the WorldClock's
//                calendar date (the day whose Sun is at this phase), keeping
//                the clock's time of day — so with a 12-minute fixed_day the
//                sun's path, day length and dusk follow the season.
//   manual       the phase holds (UI, Lua or MCP set it; "hold this season").
//
// Each tick derives a SeasonSample on the same entity (not saved): the four
// season weights, the day length from the clock's almanac, and the climate
// now (the entity's weather::Climate, or the default profile). With
// weather_auto, a seeded weather sequence over the climate writes the scene's
// WeatherState (wind, clouds, precipitation, temperature; created when the
// scene has none), and R13.1's step_weather accumulates wetness and snow from
// it. A manual weather.set suspends weather_auto.

#include <array>
#include <cstdint>
#include <optional>
#include <span>
#include <string_view>

#include <entt/entity/fwd.hpp>

#include "engine/astro/time.hpp"
#include "engine/weather/climate.hpp"

namespace engine::scene {

struct WorldClock;

enum class SeasonMode : std::uint8_t { clock, independent, manual };

struct SeasonState {
    bool       enabled = true;
    SeasonMode mode = SeasonMode::clock;
    // [0, 1): state in independent / manual mode, derived in clock mode.
    double year_phase = 0.0;
    // The season year (the calendar year of its spring equinox): counts
    // wraps in independent mode, varies the weather from year to year, and
    // anchors the date drive_clock_date writes.
    std::int32_t year = 2026;
    // Years per simulated second in independent mode (1 / 2400: 40 minutes).
    double phase_rate = 1.0 / 2400.0;
    // Independent / manual: write the WorldClock's date from the phase.
    bool drive_clock_date = false;
    // Drive the scene's WeatherState from the climate's weather sequence.
    bool          weather_auto = true;
    std::uint32_t weather_seed = 1;

    friend bool operator==(const SeasonState&, const SeasonState&) = default;
};

inline constexpr double k_max_phase_rate = 1.0; // a year per simulated second

enum class Season : std::uint8_t { spring, summer, autumn, winter };
inline constexpr std::array<std::string_view, 4> k_season_names = {"spring", "summer", "autumn",
                                                                   "winter"};
// The phase of the middle of each season (0.125, 0.375, 0.625, 0.875).
[[nodiscard]] double season_centre(Season season);
[[nodiscard]] bool   parse_season(std::string_view name, Season& out);
[[nodiscard]] std::string_view season_mode_name(SeasonMode mode);
[[nodiscard]] bool             parse_season_mode(std::string_view name, SeasonMode& out);

// Derived each tick (not saved).
struct SeasonSample {
    double year_phase = 0.0;
    // spring, summer, autumn, winter: Hann windows one season wide centred
    // on each season's middle; they overlap pairwise and sum to 1.
    std::array<float, 4> weights{};
    bool  southern = false;   // the clock is south of the equator
    bool  has_clock = false;  // day length and solar time come from a clock
    float day_length_h = 12.0F;
    bool  polar_day = false;
    bool  polar_night = false;
    astro::UtcTime almanac_day{-1}; // local midnight the day length belongs to
    // The climate now (time of day included when there is a clock).
    weather::ClimateSample climate;
    bool                   custom_climate = false; // the entity has a Climate
    double                 season_days = 0.0;      // days since season day 0
    // The weather sequence (valid while weather_auto runs).
    weather::WeatherDay weather_day;
    std::int64_t        weather_period = 0;
    float               weather_blend = 0.0F;
};

// ---- Astronomy of the year ---------------------------------------------------------

// Year phase of `utc` at `latitude_deg`: the Sun's apparent ecliptic
// longitude / 360, + 0.5 in the southern hemisphere, wrapped to [0, 1).
[[nodiscard]] double year_phase_at(astro::UtcTime utc, double latitude_deg);

// The instant in season year `year` (the one starting at that calendar
// year's local spring equinox) at which the year phase is `phase`, for the
// given hemisphere. Solved on R22.1's Sun to well under a second.
[[nodiscard]] astro::UtcTime instant_of_phase(std::int32_t year, double phase, bool southern);

// The season year `utc` falls in (the year of the latest local spring
// equinox at or before it).
[[nodiscard]] std::int32_t season_year_at(astro::UtcTime utc, bool southern);

// Days since season day 0 (the 2000 season year's start): year × tropical
// year + phase × tropical year; indexes the weather sequence.
[[nodiscard]] double season_days(std::int32_t year, double phase);

[[nodiscard]] std::array<float, 4> season_weights(double phase);

// `clock.utc` moved to the local date whose Sun is at `phase` of season year
// `year`, keeping the local time of day. Clamped to the supported years.
[[nodiscard]] astro::UtcTime clock_date_for_phase(const WorldClock& clock, std::int32_t year,
                                                  double phase);

// ---- ECS ----------------------------------------------------------------------------

[[nodiscard]] entt::entity find_season(const entt::registry& registry);

// Advances / derives the season (see the header comment). Called from
// Scene::tick after world_clock_system with the simulation dt.
void season_system(entt::registry& registry, float dt);

// Re-derives the sample (and drives the clock date and weather) without
// advancing time: after an edit, so the next read sees the new state.
void apply_season(entt::registry& registry);

// Runs the weather, clock and season systems over the `seconds` of
// simulated time that lead to the current state (rewinding first), in steps
// of `step` seconds, so wetness and snow have accumulated as if the scene had
// lived through them; then restores the exact phase and clock. For screenshots
// and season jumps. `seconds` is limited to k_max_spin_up_s.
inline constexpr double k_max_spin_up_s = 3600.0;
void spin_up_season(entt::registry& registry, double seconds, float step = 1.0F);

} // namespace engine::scene
