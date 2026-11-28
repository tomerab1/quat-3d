The scene's weather: `{present, entity, weather: {wind_dir_deg, wind_speed, gustiness, turbulence, cloud_cover, precipitation, precipitation_type, wetness, snow, temperature_c, accumulate}}`. `present` is false (and `entity` null) when the scene has no weather yet; the values are then the defaults the renderer uses.

Options:
- `schema: true` adds `schema`: every field's range, default, unit and hint.
- `wind_at: [[x, y, z], ...]` adds `wind_at`: the wind velocity `[vx, vy, vz]` in m/s at each point, as the grass feels it this frame. `y` is the height above the ground. The wind is defined at 10 m and slows toward the ground (logarithmic profile).

Wind direction is the heading the wind blows toward, in degrees from +X toward +Z. The clouds use the same convention.
