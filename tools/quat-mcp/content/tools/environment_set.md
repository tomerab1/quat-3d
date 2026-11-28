Change the sun, the clouds and the sky source. Every field is optional, and omitted fields keep their values. Returns the new environment, in the same form as `environment_get`.

Examples:
- sunset: `{"sun": {"elevation_deg": 6, "azimuth_deg": 200}}`
- overcast: `{"clouds": {"enabled": true, "coverage": 0.85, "density": 40}}`
- clear sky: `{"clouds": {"enabled": false}}`
- light the scene with a panorama: `{"sky": "hdri", "hdri": {"path": "assets/hdri/meadow_2k.hdr"}}`
- turn the panorama so its sun rakes the scene, a bit darker: `{"hdri": {"rotation_deg": 120, "intensity": 0.8}}`
- back to the procedural sky: `{"sky": "procedural"}`

An HDRI (`hdri.path`) is a Radiance `.hdr` equirectangular panorama inside the project (for example a CC0 Poly Haven download under `assets/hdri/`). It replaces the sky for the background, the ambient light and the reflections. With `hdri.sun` on (the default), the panorama's sun becomes the shadow-casting sun light. It is removed from the panorama so its light is not counted twice. While `sky` is `hdri`, the `sun` fields have no effect. A file that fails to load is reported in the error, and `environment_get` shows it under `hdri.error`.
