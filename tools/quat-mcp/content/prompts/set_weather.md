Set the weather of the scene in the running quat-3d engine, using the quat-mcp tools.
{%- if description %}

The weather to reach: {{ description }}
{%- endif %}

1. **Look first.** `camera_frame` (or `camera_set`) to a view with ground, vegetation and sky in it, and take a `screenshot`. `weather_get` shows the current weather; `present: false` means the scene has none yet and runs on the defaults. `weather_get` with `schema: true` lists every field's range.
2. **Sky and wind.** Set `cloud_cover` and the wind with `weather_set`. `wind_speed` is in m/s: 2 is a breeze, 8 fresh, 15 strong, 25 a storm. `gustiness` sends gust fronts across the grass and `turbulence` adds swirl. `wind_dir_deg` is the heading it blows toward. `weather_get` with `wind_at` checks the wind at a point.
3. **Precipitation and surfaces.** `precipitation` with `precipitation_type` ("rain" or "snow") sets what falls. Falling rain and snow are not drawn yet; for now precipitation drives accumulation. Surfaces respond over simulated time while `accumulate` is on. For the final look now, set `wetness` (0.3 damp, 0.6 wet with the first puddles, 1 flooded) or `snow` (0.3 a dusting, 0.8 deep) directly, with a matching `temperature_c` (below 0 for snow that should stay).
4. **Keep it coherent.** Rain wants high cloud cover, snow wants cold air and lighter wind, and a storm wants gusts. The sun stays where `environment_set` puts it; overcast skies already dim it through cloud shadows.
5. **Check.** Take another `screenshot` from the same camera and compare. Change one field at a time.
6. **Finish** with the fields you set and why. `scene_save` stores the weather with the scene; save only if asked.
