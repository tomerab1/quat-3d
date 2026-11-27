Change renderer settings; the next frame renders with them, so follow up with `screenshot`. Give only the groups and fields that change — everything else keeps its value. The engine range-checks every value and rejects the whole call (changing nothing) if any field is wrong; the error names the field and its range. Returns every group's new values.

Examples:
- brighter, fixed exposure: `{"exposure": {"auto": false, "compensation_ev": 0.5}}`
- keep auto exposure but darken one stop: `{"exposure": {"compensation_ev": -1}}`
- softer, wider glow: `{"bloom": {"intensity": 0.12, "radius": 1.5, "threshold": 0.8}}`
- softer shadow edges and no acne: `{"shadows": {"pcf_radius": 2, "depth_bias": 0.0015}}`
- dimmer sky light: `{"ibl": {"intensity": 0.6}}`

Change one group at a time and compare screenshots; `render_settings_reset` undoes a group.
