Set the scene's date, time, place or clock speed; every field is optional and omitted fields keep their values. The first call creates the world clock (and a sun light if the scene has none). Returns the new state (as `time_get`).

While the clock is enabled with `drive_sun`, the sun light follows the real Sun for that date and place: it rises in the east, sets in the west, reddens through the atmosphere at low elevation and goes dark at night. `environment_set` then refuses sun angles and intensity: use this tool (or `time_skip`) instead, or set `drive_sun: false` to pose the sun by hand. An HDRI with its own sun (`environment_set` `sky: "hdri"`, `hdri.sun` on) outranks the clock: the clock keeps running, but the panorama's sun lights the scene until the HDRI or its sun is turned off.

The clock advances with the simulation (paused by `debug_pause`, slowed by `debug_time_scale`) times its own `time_scale`.

Examples:
- sunrise in Paris at midsummer: `{"datetime": "2026-06-21T05:50", "utc_offset_h": 2, "latitude_deg": 48.86, "longitude_deg": 2.35}`
- golden hour, same day: `{"local_time": "21:15"}`
- a day passing in two minutes: `{"time_scale": 720}`
- hold the light still: `{"time_scale": 0}`
- loop one day forever: `{"fixed_day": true, "time_scale": 600}`
- turn the clock off (the sun keeps its last position): `{"enabled": false}`
