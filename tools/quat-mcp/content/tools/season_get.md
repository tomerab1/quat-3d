The scene's season and the climate driving its weather: `{in_scene, enabled, mode, year_phase, year, phase_rate, year_minutes, day_minutes, drive_clock_date, weather_auto, weather_seed, weather_clock_time, season, weights, has_clock, southern, day_length_h, polar, climate: {...}, weather: {...}}`.

- `year_phase` runs 0..1 from the local spring equinox: 0.25 is the summer solstice, 0.5 the autumn equinox, 0.75 the winter solstice. South of the equator it is shifted by half a year, so "summer" is always the warm season.
- `mode`: `clock` follows the world clock's Sun (real dates), `independent` runs its own year (`year_minutes` real minutes per year), `manual` holds.
- `season` is the dominant one; `weights` give `spring`, `summer`, `autumn`, `winter` (they sum to 1 and blend smoothly between neighbours).
- `day_minutes` is the world clock's speed as real minutes per day; `day_length_h` is sunrise to sunset on the clock's date (null without a clock).
- `climate` is the climate now: `mean_temperature_c`, `diurnal_range_c`, `temperature_c` (with the time of day), `precipitation_chance`, `cloud_cover`, `wind_speed`, `snow_fraction` (share of precipitation falling as snow), and `custom` (false: the default cool temperate profile).
- `weather` (while `weather_auto` runs) is the seeded sequence's current period: `kind` (clear, fair, overcast, showers, rain, storm, snow), `precipitation_type`, `temperature_anomaly_c`, `period`, `blend_to_next`.

`climate: true` adds `climate_curves`, the curves in use (the values to edit with `season_set`).

Without a season (`in_scene: false`) the values describe the defaults that `season_set` would create.
