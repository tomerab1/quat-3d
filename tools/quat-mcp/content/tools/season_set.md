Set the scene's season, its year speed and the climate that drives the weather; every field is optional and omitted fields keep their values. The first call creates the season. Returns the new state (as `season_get`).

The season drives `WeatherState` (wind, clouds, rain or snow, temperature) from its climate through a seeded weather sequence while `weather_auto` is on, and the weather's own accumulation then wets the ground and lays or melts snow. A manual `weather_set` suspends `weather_auto`; set it back to `true` to resume. With `weather_clock_time` (the default) wetness and snow evolve at the world clock's speed, so a fast day dries and thaws at the pace its sun moves.

Modes:
- `clock`: the season follows the world clock's date (`time_set`), with real equinoxes, solstices and day lengths for its latitude. `season` or `year_phase` then move the clock's date and keep its time of day.
- `independent`: the year runs at `year_minutes`, decoupled from the day. With `drive_clock_date` the clock's date follows the season, so the sun's path and day length change with it while the clock loops its day.
- `manual`: hold the season where it is set.

Examples:
- a cozy accelerated year (40-minute year, 12-minute day, the sun following the season): `{"mode": "independent", "year_minutes": 40, "day_minutes": 12, "drive_clock_date": true}` (needs a world clock: `time_set` first)
- follow the real calendar: `{"real_calendar": true}`
- hold mid-autumn: `{"mode": "manual", "season": "autumn"}`
- show winter with snow already lying: `{"mode": "manual", "season": "winter", "drive_clock_date": true, "spin_up_s": 300}`
- a storm that suits the season (snow in a cold winter): `{"weather_preset": "storm"}`
- a colder climate: `{"climate": {"temperature_c": [0, 6, 12, 14, 10, 4, -3, -8]}}`; back to the default with `{"default_climate": true}`

Climate curves take 1 to 48 values each, evenly spaced over the year from the spring equinox (key i at phase i / n). Take a screenshot from the same camera before and after to compare.
