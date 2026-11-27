Change the sun and/or clouds; every field is optional and omitted fields keep their values. Returns the new environment (as `environment_get`).

Examples:
- sunset: `{"sun": {"elevation_deg": 6, "azimuth_deg": 200}}`
- overcast: `{"clouds": {"enabled": true, "coverage": 0.85, "density": 40}}`
- clear sky: `{"clouds": {"enabled": false}}`
