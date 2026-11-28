---
name: set-weather
description: Set the weather of a running quat-3d scene - wind, gusts, cloud cover, rain or snow, wet and snowy surfaces, temperature - and check it with screenshots from a fixed camera. Use when the user asks for a windy, rainy, stormy, overcast or snowy scene, wet streets, a dusting of snow or calmer air.
compatibility: Needs the quat-mcp MCP server connected to a running quat-3d engine (started with QUAT_REMOTE).
metadata:
  quat-mcp-tools: "camera_frame camera_set screenshot weather_get weather_set environment_set scene_save"
---

# Set the weather

The scene has at most one weather. Without one it renders on the defaults
(no cloud shadows, grass in its plain sway); `weather_set` creates it.

## Workflow

1. **Look first.** `camera_frame` (or `camera_set`) to a view with ground,
   vegetation and sky in it, and take a `screenshot`. `weather_get` shows the
   current weather; `present: false` means the scene has none yet.
   `weather_get` with `schema: true` lists every field's range, unit and
   default.
2. **Sky and wind.** Set `cloud_cover` and the wind with `weather_set`.
   `wind_speed` is in m/s at 10 m: 2 is a breeze, 8 fresh, 15 strong, 25 a
   storm. `gustiness` sends gust fronts across the grass and `turbulence`
   adds swirl. `wind_dir_deg` is the heading the wind blows toward.
   `weather_get` with `wind_at` checks the wind at given points.
3. **Precipitation and surfaces.** `precipitation` with `precipitation_type`
   ("rain" or "snow") sets what falls. Falling rain and snow are not drawn
   yet: precipitation drives accumulation over simulated time while
   `accumulate` is on. For the final look now, set `wetness` (0.3 damp, 0.6
   wet with the first puddles, 1 flooded) or `snow` (0.3 a dusting, 0.8
   deep) directly, with a matching `temperature_c` (below 0 for snow that
   should stay).
4. **Keep it coherent.** Rain wants high cloud cover, snow wants cold air
   and lighter wind, and a storm wants gusts. The sun stays where
   `environment_set` puts it; overcast skies already dim it through cloud
   shadows.
5. **Check.** Take another `screenshot` from the same camera and compare.
   Change one field at a time.
6. **Finish** with the fields you set and why. `scene_save` stores the
   weather with the scene; save only if asked.

## Pitfalls

- A second weather entity is refused: edit the scene's one with
  `weather_set` instead of adding the component elsewhere.
- `weather_set` merges fields; `reset: true` returns to the defaults first.
- Surfaces keep changing while `accumulate` is on and the simulation runs:
  turn it off to hold a look you set by hand.
