Current sky: `{sun: {azimuth_deg, elevation_deg, intensity, color}, clouds: {enabled, coverage, density, bottom_km, thickness_km, size_km, detail, wind_speed, wind_dir_deg}, sky: "procedural" | "hdri", hdri: {path, loaded, width, height, sun_found, error?, intensity, rotation_deg, sun}}`.

Sun angles: `azimuth_deg = atan2(d.z, d.x)` and `elevation_deg = asin(-d.y)` of the light's travel direction d. Elevation 90 is noon overhead, 5..10 is golden hour, and below 0 is night.

`sky` is `hdri` only after a panorama was requested (`environment_set`). It renders only while `hdri.loaded` is true; until then the procedural sky stands in. `sun_found` tells whether the panorama has a sun that can drive the sun light.
