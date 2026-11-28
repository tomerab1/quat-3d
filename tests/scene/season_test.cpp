// SeasonState (R25.1): the year phase from the Sun in both hemispheres, the
// independent (accelerated) year and its clock date, weights, day length,
// the climate-driven weather and determinism.

#include <cmath>
#include <cstring>

#include <entt/entity/registry.hpp>
#include <gtest/gtest.h>

#include "engine/astro/events.hpp"
#include "engine/astro/time.hpp"
#include "engine/scene/season.hpp"
#include "engine/scene/world_clock.hpp"
#include "engine/weather/climate.hpp"
#include "engine/weather/weather_state.hpp"

namespace {

namespace a = engine::astro;
namespace s = engine::scene;
namespace w = engine::weather;

[[nodiscard]] a::UtcTime utc(int y, int mo, int d, int h = 0, int mi = 0) {
    return a::utc_from_civil({y, mo, d, h, mi, 0.0}, 0);
}

// Signed distance between two phases, in [-0.5, 0.5).
[[nodiscard]] double phase_diff(double x, double y) {
    double d = std::fmod(x - y + 0.5, 1.0);
    if (d < 0.0) d += 1.0;
    return d - 0.5;
}

constexpr entt::entity k_none = entt::null;
constexpr double k_paris_lat = 48.8566;
constexpr double k_sydney_lat = -33.8688;
// One minute of the year, as phase.
constexpr double k_minute = 1.0 / (w::k_tropical_year_days * 1440.0);

// 2026's equinoxes and solstices (UTC, to the minute; USNO / IMCCE tables).
struct Event {
    a::UtcTime  when;
    double      north_phase;
    const char* name;
};
const Event k_events_2026[] = {
    {utc(2026, 3, 20, 14, 46), 0.0, "March equinox"},
    {utc(2026, 6, 21, 8, 24), 0.25, "June solstice"},
    {utc(2026, 9, 23, 0, 5), 0.5, "September equinox"},
    {utc(2026, 12, 21, 20, 50), 0.75, "December solstice"},
};

TEST(SeasonPhase, SolsticesAndEquinoxesInBothHemispheres) {
    for (const Event& ev : k_events_2026) {
        // Within a few minutes of the tabulated instant (the published times
        // are rounded to the minute; Meeus' low-accuracy Sun is ~0.01 deg,
        // about 15 minutes of the year).
        EXPECT_NEAR(phase_diff(s::year_phase_at(ev.when, k_paris_lat), ev.north_phase), 0.0, 20 * k_minute)
            << ev.name;
        // South of the equator the seasons are half a year apart: the March
        // equinox starts the southern autumn.
        EXPECT_NEAR(phase_diff(s::year_phase_at(ev.when, k_sydney_lat), ev.north_phase + 0.5), 0.0,
                    20 * k_minute)
            << ev.name;
        // The phase increases through the event: a day earlier is behind.
        const a::UtcTime day_before = a::add_seconds(ev.when, -86400.0);
        EXPECT_LT(phase_diff(s::year_phase_at(day_before, k_paris_lat), ev.north_phase), -0.002) << ev.name;
    }
}

TEST(SeasonPhase, InstantOfPhaseInvertsTheSun) {
    for (const bool south : {false, true}) {
        const double lat = south ? k_sydney_lat : k_paris_lat;
        const a::UtcTime start = s::instant_of_phase(2026, 0.0, south);
        const a::UtcTime next = s::instant_of_phase(2027, 0.0, south);
        // The season year is one tropical year long.
        EXPECT_NEAR(static_cast<double>(next.ns - start.ns) * 1e-9 / 86400.0, w::k_tropical_year_days, 0.05);
        for (int i = 0; i < 40; ++i) {
            const double     p = i / 40.0;
            const a::UtcTime t = s::instant_of_phase(2026, p, south);
            EXPECT_NEAR(phase_diff(s::year_phase_at(t, lat), p), 0.0, 1e-8) << p << " south " << south;
            EXPECT_GE(t, start);
            EXPECT_LT(t, next);
            EXPECT_EQ(s::season_year_at(t, south), 2026) << p;
        }
    }
    // The northern 2026 season year starts at the March equinox.
    EXPECT_NEAR(static_cast<double>(s::instant_of_phase(2026, 0.0, false).ns - k_events_2026[0].when.ns) * 1e-9,
                0.0, 20 * 60.0);
    // January belongs to the previous (northern) season year.
    EXPECT_EQ(s::season_year_at(utc(2026, 1, 10), false), 2025);
    EXPECT_EQ(s::season_year_at(utc(2026, 4, 10), false), 2026);
    // In the south the year starts at the September equinox.
    EXPECT_EQ(s::season_year_at(utc(2026, 8, 1), true), 2025);
    EXPECT_EQ(s::season_year_at(utc(2026, 10, 1), true), 2026);
}

TEST(SeasonWeights, HannWindowsPartitionTheYear) {
    for (int i = 0; i <= 1000; ++i) {
        const double p = i / 1000.0;
        const auto   wts = s::season_weights(p);
        float        sum = 0.0F;
        int          nonzero = 0;
        for (const float x : wts) {
            EXPECT_GE(x, 0.0F);
            sum += x;
            nonzero += x > 1e-6F;
        }
        EXPECT_NEAR(sum, 1.0F, 1e-5F) << p;
        EXPECT_LE(nonzero, 2) << p;
    }
    for (int k = 0; k < 4; ++k) {
        const auto season = static_cast<s::Season>(k);
        const auto wts = s::season_weights(s::season_centre(season));
        EXPECT_NEAR(wts[static_cast<std::size_t>(k)], 1.0F, 1e-6F) << k;
    }
    // At a solstice summer and spring share the weight equally.
    const auto june = s::season_weights(0.25);
    EXPECT_NEAR(june[0], 0.5F, 1e-6F);
    EXPECT_NEAR(june[1], 0.5F, 1e-6F);
}

TEST(SeasonNames, RoundTrip) {
    for (std::size_t i = 0; i < s::k_season_names.size(); ++i) {
        s::Season x{};
        ASSERT_TRUE(s::parse_season(s::k_season_names[i], x));
        EXPECT_EQ(static_cast<std::size_t>(x), i);
    }
    for (const char* m : {"clock", "independent", "manual"}) {
        s::SeasonMode mode{};
        ASSERT_TRUE(s::parse_season_mode(m, mode));
        EXPECT_EQ(s::season_mode_name(mode), m);
    }
    s::Season x{};
    EXPECT_FALSE(s::parse_season("fall", x));
}

// A registry with a clock (optional) and a season.
struct SeasonWorld {
    entt::registry r;
    entt::entity   clock = entt::null;
    entt::entity   season = entt::null;

    SeasonWorld(const s::SeasonState& st, const s::WorldClock* c) {
        if (c != nullptr) {
            clock = r.create();
            r.emplace<s::WorldClock>(clock, *c);
            s::apply_world_clock(r);
        }
        season = r.create();
        r.emplace<s::SeasonState>(season, st);
        s::apply_season(r);
    }
    // Scene::tick's order for the systems involved.
    void tick(float dt) {
        w::weather_system(r, s::weather_dt(r, dt));
        s::world_clock_system(r, dt);
        s::season_system(r, dt);
    }
    s::SeasonState&        state() { return r.get<s::SeasonState>(season); }
    const s::SeasonSample& sample() { return r.get<s::SeasonSample>(season); }
    s::WorldClock&         world_clock() { return r.get<s::WorldClock>(clock); }
    w::WeatherState&       weather() { return r.get<w::WeatherState>(w::find_weather(r)); }
};

TEST(SeasonClockMode, FollowsTheClockWithItsDaylight) {
    s::WorldClock paris;
    paris.utc = k_events_2026[1].when; // June solstice
    paris.time_scale = 0.0;
    SeasonWorld north({}, &paris);
    EXPECT_NEAR(phase_diff(north.state().year_phase, 0.25), 0.0, 20 * k_minute);
    EXPECT_EQ(north.state().year, 2026);
    EXPECT_FALSE(north.sample().southern);
    // Paris at midsummer: ~16 h 10 min between sunrise and sunset.
    EXPECT_NEAR(north.sample().day_length_h, 16.2F, 0.15F);

    s::WorldClock sydney = paris;
    sydney.latitude_deg = k_sydney_lat;
    sydney.longitude_deg = 151.2093;
    sydney.utc_offset_min = 600;
    SeasonWorld south({}, &sydney);
    EXPECT_NEAR(phase_diff(south.state().year_phase, 0.75), 0.0, 20 * k_minute);
    EXPECT_TRUE(south.sample().southern);
    EXPECT_NEAR(south.sample().weights[3], 0.5F, 0.01F); // winter begins
    EXPECT_NEAR(south.sample().day_length_h, 9.9F, 0.15F);
    // Mid-winter is colder than midsummer in the default climate.
    EXPECT_LT(south.sample().climate.mean_temperature_c, north.sample().climate.mean_temperature_c);
}

TEST(SeasonClockMode, WithoutAClockThePhaseHolds) {
    s::SeasonState st;
    st.year_phase = 0.6;
    SeasonWorld world(st, nullptr);
    world.tick(100.0F);
    EXPECT_EQ(world.state().year_phase, 0.6);
    EXPECT_FALSE(world.sample().has_clock);
}

TEST(SeasonIndependent, AcceleratedYearStaysConsistentWithTheClock) {
    // The cozy builder's default: a 40-minute year, a 12-minute looping day,
    // the season writing the clock's date.
    s::WorldClock c;
    c.utc = a::utc_from_civil({2026, 4, 1, 9, 0, 0.0}, 120);
    c.time_scale = 1440.0 / 12.0;
    c.fixed_day = true;
    s::SeasonState st;
    st.mode = s::SeasonMode::independent;
    st.year_phase = 0.0;
    st.year = 2026;
    st.phase_rate = 1.0 / (40.0 * 60.0);
    st.drive_clock_date = true;
    SeasonWorld world(st, &c);
    const std::int64_t start_tod =
        world.world_clock().utc.ns - a::local_midnight(world.world_clock().utc, 120).ns;

    const float dt = 1.0F / 30.0F;
    const int   steps = 30 * 600; // ten minutes: a quarter of the year
    double      sim = 0.0;
    for (int i = 0; i < steps; ++i) {
        world.tick(dt);
        sim += dt;
        if (i % 600 == 0) {
            // The clock's own season (from its Sun) agrees with the phase to
            // within a day: the date follows the phase, the clock keeps its
            // time of day.
            const s::WorldClock& clk = world.world_clock();
            EXPECT_NEAR(phase_diff(s::year_phase_at(clk.utc, clk.latitude_deg), world.state().year_phase), 0.0,
                        1.05 / w::k_tropical_year_days)
                << "t = " << sim;
        }
    }
    EXPECT_NEAR(world.state().year_phase, sim / 2400.0, 1e-9);
    EXPECT_NEAR(world.state().year_phase, 0.25, 1e-5);
    // The day looped: 600 s x 120 = 20 h of clock time after 09:00 local.
    const s::WorldClock& clk = world.world_clock();
    const std::int64_t   tod = clk.utc.ns - a::local_midnight(clk.utc, 120).ns;
    const double         expected_tod = std::fmod(static_cast<double>(start_tod) * 1e-9 + sim * 120.0, 86400.0);
    EXPECT_NEAR(static_cast<double>(tod) * 1e-9, expected_tod, 0.5);
    // ... on the summer solstice's date.
    const a::CivilTime civil = a::civil_from_utc(clk.utc, 120);
    EXPECT_EQ(civil.month, 6);
    EXPECT_NEAR(civil.day, 21, 1);
    // The days are long now: Paris midsummer.
    EXPECT_GT(world.sample().day_length_h, 16.0F);

    // A whole year wraps the phase and counts the year.
    for (int i = 0; i < 30 * 1800; ++i) world.tick(dt);
    EXPECT_EQ(world.state().year, 2027);
    EXPECT_LT(world.state().year_phase, 1e-3);
}

TEST(SeasonIndependent, ManualHoldsAndDrivesTheDate) {
    s::WorldClock c;
    c.time_scale = 0.0;
    s::SeasonState st;
    st.mode = s::SeasonMode::manual;
    st.year_phase = s::season_centre(s::Season::winter);
    st.drive_clock_date = true;
    SeasonWorld world(st, &c);
    world.tick(500.0F);
    EXPECT_EQ(world.state().year_phase, s::season_centre(s::Season::winter));
    // Mid-winter of season year 2026 is early February 2027 in Paris.
    const a::CivilTime civil = a::civil_from_utc(world.world_clock().utc, c.utc_offset_min);
    EXPECT_EQ(civil.year, 2027);
    EXPECT_EQ(civil.month, 2);
    EXPECT_LT(world.sample().day_length_h, 10.5F);
    EXPECT_NEAR(world.sample().weights[3], 1.0F, 1e-6F);
}

TEST(SeasonWeather, AutoWeatherFollowsTheClimateAndSnowArrivesAndMelts) {
    // A four-hour year at one-second steps, no clock: 122 weather periods.
    s::SeasonState st;
    st.mode = s::SeasonMode::independent;
    st.phase_rate = 1.0 / (4.0 * 3600.0);
    st.weather_seed = 3;
    SeasonWorld world(st, nullptr);
    ASSERT_NE(w::find_weather(world.r), k_none) << "weather_auto creates the scene's weather";

    float max_winter_snow = 0.0F;
    float max_summer_snow = 0.0F;
    float summer_temp = 0.0F;
    float winter_temp = 0.0F;
    int   summer_n = 0;
    int   winter_n = 0;
    int   snowing = 0;
    for (int i = 0; i < 4 * 3600; ++i) {
        world.tick(1.0F);
        const double         p = world.state().year_phase;
        const w::WeatherState& ws = world.weather();
        if (p > 0.75) {
            max_winter_snow = std::max(max_winter_snow, ws.snow);
            winter_temp += ws.temperature_c;
            ++winter_n;
            snowing += ws.precipitation > 0.0F && ws.precipitation_type == w::PrecipitationType::snow;
        } else if (p > 0.3 && p < 0.45) {
            max_summer_snow = std::max(max_summer_snow, ws.snow);
            summer_temp += ws.temperature_c;
            ++summer_n;
            EXPECT_EQ(ws.precipitation_type == w::PrecipitationType::snow && ws.precipitation > 0.0F, false)
                << "snow in midsummer at phase " << p;
        }
    }
    EXPECT_GT(snowing, 0);
    EXPECT_GT(max_winter_snow, 0.1F) << "winter snow settles through step_weather";
    // step_weather melts exponentially: a trace is left, not a cover.
    EXPECT_LT(max_summer_snow, 0.01F) << "and has melted by midsummer";
    EXPECT_GT(summer_temp / static_cast<float>(summer_n), winter_temp / static_cast<float>(winter_n) + 10.0F);
}

TEST(SeasonWeather, ManualWeatherAutoOffLeavesTheWeatherAlone) {
    s::SeasonState st;
    st.weather_auto = false;
    st.mode = s::SeasonMode::independent;
    SeasonWorld world(st, nullptr);
    EXPECT_EQ(w::find_weather(world.r), k_none);
}

[[nodiscard]] bool same_bytes(const w::WeatherState& x, const w::WeatherState& y) {
    return std::memcmp(&x, &y, sizeof x) == 0;
}

TEST(SeasonDeterminism, SameDtSequenceSameStateBitForBit) {
    s::WorldClock c;
    c.time_scale = 120.0;
    c.fixed_day = true;
    s::SeasonState st;
    st.mode = s::SeasonMode::independent;
    st.phase_rate = 1.0 / 2400.0;
    st.drive_clock_date = true;
    st.weather_seed = 42;
    SeasonWorld one(st, &c);
    SeasonWorld two(st, &c);
    // An irregular but identical dt sequence.
    for (int i = 0; i < 3000; ++i) {
        const float dt = 1.0F / 60.0F + static_cast<float>(i % 7) * 0.003F;
        one.tick(dt);
        two.tick(dt);
    }
    EXPECT_EQ(one.state(), two.state());
    EXPECT_EQ(one.world_clock().utc, two.world_clock().utc);
    EXPECT_TRUE(same_bytes(one.weather(), two.weather()));
    EXPECT_EQ(one.sample().weather_period, two.sample().weather_period);

    // Another seed: another weather.
    st.weather_seed = 43;
    SeasonWorld three(st, &c);
    int differ = 0;
    for (int i = 0; i < 3000; ++i) {
        const float dt = 1.0F / 60.0F + static_cast<float>(i % 7) * 0.003F;
        three.tick(dt);
        one.tick(dt);
        differ += !same_bytes(one.weather(), three.weather());
    }
    EXPECT_GT(differ, 0);
}

TEST(SeasonSpinUp, AccumulatesThenLandsExactlyOnTheState) {
    s::WorldClock c;
    c.time_scale = 120.0;
    c.fixed_day = true;
    c.utc = a::utc_from_civil({2026, 7, 1, 11, 0, 0.0}, 120);
    s::SeasonState st;
    st.mode = s::SeasonMode::manual;
    st.year_phase = 0.86;
    st.phase_rate = 1.0 / 2400.0;
    st.drive_clock_date = true;
    st.weather_seed = 5;
    SeasonWorld one(st, &c);
    SeasonWorld two(st, &c);
    const s::WorldClock before_clock = one.world_clock();
    s::spin_up_season(one.r, 600.0);
    s::spin_up_season(two.r, 600.0);
    EXPECT_EQ(one.state(), st) << "mode, phase and year restored exactly";
    EXPECT_EQ(one.world_clock().utc, before_clock.utc);
    EXPECT_TRUE(same_bytes(one.weather(), two.weather()));
    // Ten minutes of a cold winter left some snow or wet ground behind.
    EXPECT_GT(one.weather().snow + one.weather().wetness, 0.0F);
    // Nothing to do without time.
    const w::WeatherState held = one.weather();
    s::spin_up_season(one.r, 0.0);
    EXPECT_TRUE(same_bytes(one.weather(), held));
}

TEST(SeasonWeather, SurfacesEvolveInWorldClockTime) {
    s::WorldClock c;
    c.time_scale = 120.0; // a 12-minute day
    SeasonWorld world({}, &c);
    EXPECT_FLOAT_EQ(s::weather_dt(world.r, 0.5F), 60.0F);
    world.state().weather_clock_time = false;
    EXPECT_FLOAT_EQ(s::weather_dt(world.r, 0.5F), 0.5F);
    world.state().weather_clock_time = true;
    world.world_clock().enabled = false;
    EXPECT_FLOAT_EQ(s::weather_dt(world.r, 0.5F), 0.5F);
    world.world_clock().enabled = true;
    world.state().enabled = false;
    EXPECT_FLOAT_EQ(s::weather_dt(world.r, 0.5F), 0.5F);
    world.state().enabled = true;
    entt::registry empty;
    EXPECT_FLOAT_EQ(s::weather_dt(empty, 0.5F), 0.5F);

    // The cozy day: a snowfall covers the ground within a minute of
    // simulation (hours of clock time), and a summer thaw clears it as fast.
    world.state().mode = s::SeasonMode::manual;
    world.state().year_phase = s::season_centre(s::Season::winter);
    s::apply_season(world.r);
    s::apply_weather_preset(world.r, w::WeatherKind::snow);
    EXPECT_FALSE(world.state().weather_auto) << "a preset suspends the automatic weather";
    for (int i = 0; i < 60; ++i) world.tick(1.0F);
    EXPECT_GT(world.weather().snow, 0.5F);
    world.state().year_phase = s::season_centre(s::Season::summer);
    world.state().weather_auto = true;
    s::apply_season(world.r);
    for (int i = 0; i < 60; ++i) world.tick(1.0F);
    EXPECT_LT(world.weather().snow, 0.01F);
}

TEST(SeasonStale, DisabledOrRemovedSeasonDropsItsSample) {
    SeasonWorld world({}, nullptr);
    ASSERT_TRUE(world.r.all_of<s::SeasonSample>(world.season));
    world.state().enabled = false;
    s::apply_season(world.r);
    EXPECT_FALSE(world.r.all_of<s::SeasonSample>(world.season));
    world.state().enabled = true;
    s::apply_season(world.r);
    world.r.remove<s::SeasonState>(world.season);
    s::apply_season(world.r);
    EXPECT_FALSE(world.r.all_of<s::SeasonSample>(world.season));
}

} // namespace
