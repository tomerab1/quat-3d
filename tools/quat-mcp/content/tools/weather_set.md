Change the scene's weather. Every field is optional and omitted fields keep their values; `reset: true` starts from the defaults. The scene gets a weather entity the first time. Returns the new weather (as `weather_get`).

The weather drives:
- the wind the grass bends in (gust fronts sweep downwind);
- the cloud cover, the clouds' drift and the clouds' shadows on the ground;
- wet surfaces: darker and glossier, with puddles on flat ground once `wetness` passes about 0.55;
- snow lying on upward-facing surfaces (it also lowers the terrain's snowline).

With `accumulate` on (the default), surfaces change over simulated time. Rain soaks them within a minute or two. Snow settles at or below 1 C. Warmth melts snow into wetness, and dry weather slowly dries everything. For an immediate look, also set `wetness` or `snow` directly.

Examples:
- calm, clear morning: `{"wind_speed": 1.5, "gustiness": 0.1, "cloud_cover": 0.1, "precipitation": 0, "wetness": 0}`
- windy and wet after a shower: `{"wind_speed": 14, "gustiness": 0.8, "turbulence": 0.5, "cloud_cover": 0.75, "wetness": 0.85}`
- heavy rain: `{"precipitation": 0.9, "precipitation_type": "rain", "cloud_cover": 0.95, "wetness": 1}`
- snowy: `{"precipitation": 0.5, "precipitation_type": "snow", "temperature_c": -5, "snow": 0.8, "cloud_cover": 0.8, "wind_speed": 3}`
