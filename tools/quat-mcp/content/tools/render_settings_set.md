Change renderer settings; the next frame renders with them, so follow up with `screenshot`. Give only the groups and fields that change — everything else keeps its value. The engine range-checks every value and rejects the whole call (changing nothing) if any field is wrong; the error names the field and its range. Returns every group's new values.

Examples:
- brighter, fixed exposure: `{"exposure": {"auto": false, "compensation_ev": 0.5}}`
- keep auto exposure but darken one stop: `{"exposure": {"compensation_ev": -1}}`
- softer, wider glow: `{"bloom": {"intensity": 0.12, "radius": 1.5, "threshold": 0.8}}`
- softer shadow edges and no acne: `{"shadows": {"pcf_radius": 2.5, "normal_bias_texels": 1.5}}`
- shadows further out, coarser near the camera: `{"shadows": {"max_distance": 400, "split_lambda": 0.6}}`
- cheaper shadows in dense foliage (small casters stop casting into the coarse far cascades): `{"shadows": {"caster_min_texels": 3}}`; per model, `instanced_mesh.shadow_distance` stops far placements casting at all. `{"shadows": {"gpu_culling": false}}` draws every caster into every cascade (compare with `profile_frame`)
- stronger, wider ambient occlusion: `{"ao": {"radius": 1.5, "strength": 3}}`
- dimmer sky light: `{"ibl": {"intensity": 0.6}}`
- sharper, longer reflections on glossy floors: `{"ssr": {"quality": "high", "max_roughness": 0.7}}`
- compare the old single-scattering metals: `{"ibl": {"multiscatter": false}}`
- render at half resolution per axis and upscale (needs TAA on): `{"upscaler": {"mode": "performance"}}`
- filmic highlights: `{"tonemap": {"operator": "agx"}}`
- shallow depth of field (the active camera's lens and focus decide the blur): `{"dof": {"enabled": true}}`
- warm golden-hour grade: `{"grading": {"temperature_k": 5200, "contrast": 1.1, "gain_r": 1.04, "lift_b": 0.02}}`
- a look LUT from the project: `{"grading": {"lut": "assets/luts/teal_orange.cube", "lut_contribution": 0.7}}` (the file must be a valid `.cube` inside the project; `""` removes it)
- lens character: `{"film": {"vignette_natural": 1, "grain_intensity": 0.25}}`
- exposure from the camera (aperture, shutter, ISO on its `camera` component): `{"exposure": {"physical": true}}`

Change one group at a time and compare screenshots; `render_settings_reset` undoes a group.
