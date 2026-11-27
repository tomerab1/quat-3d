The scene's world clock and the sky it computes: `{in_scene, enabled, datetime, utc, utc_offset_h, latitude_deg, longitude_deg, north_offset_deg, time_scale, fixed_day, drive_sun, sun_intensity, julian_date, local_sidereal_time_deg, sun: {...}, moon: {...}}`.

- `sun`: `azimuth_deg` (compass: 0 north, 90 east), `elevation_deg` (apparent; < 0 = below the horizon), `direction` (world unit vector toward the Sun), `illuminance_lux` (outside the atmosphere), `visible_fraction` of the disc above the horizon, and the local day's `sunrise`, `sunset`, `solar_noon`, `civil_dawn`, `civil_dusk`, `day_length_h` (local ISO-8601 times; null when the event does not happen that day, with `polar` = "day" or "night").
- `moon`: `azimuth_deg`, `elevation_deg`, `direction`, `illuminated_fraction` (0 new .. 1 full), `phase` (e.g. "waxing gibbous"), `phase_angle_deg`, `illuminance_lux`, `distance_km`, `moonrise`, `moonset`.

World axes: with `north_offset_deg` 0, north is -Z, east is +X, up is +Y.

Without a clock (`in_scene: false`) the values describe the default clock that `time_set` would create.
