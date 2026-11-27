Current sky: `{sun: {azimuth_deg, elevation_deg, intensity, color}, clouds: {enabled, coverage, density, bottom_km, thickness_km, size_km, detail, wind_speed, wind_dir_deg}}`.

Sun angles: `azimuth_deg = atan2(d.z, d.x)` and `elevation_deg = asin(-d.y)` of the light's travel direction d, so elevation 90 = noon overhead, 5..10 = golden hour, < 0 = night.
