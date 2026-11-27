Tune how the scene in the running quat-3d engine looks, using the quat-mcp tools.
{%- if goal %}

The goal: {{ goal }}
{%- endif %}
{%- if entity %}

Material to focus on: {{ entity }}
{%- endif %}

Work like a colourist: look, change one thing, look again.

1. **Look first.** `camera_frame` (or `camera_set`) to a representative view and take a `screenshot`. Describe what you see: overall brightness, clipped highlights or crushed shadows, glow, shadow quality, sky, how the materials read.
2. **Read the state.** `render_settings_get` for the current values and `render_settings_schema` once for every field's range, unit and default; `environment_get` for the sun and clouds.
3. **Exposure first.** Wrong brightness makes every other judgement misleading. With `auto` on, adjust `compensation_ev` in small steps (0.3 to 1 EV); touch `key`, `min_exposure` or `max_exposure` only if auto exposure pumps or overreacts. Turn `auto` off for a fixed, repeatable exposure (exposure = 2^compensation_ev).
4. **Then tonemap and bloom.** The tonemap curve is fixed (ACES filmic), so highlight roll-off follows from exposure; tune `bloom` (threshold, intensity, radius) until highlights glow without a haze over the image. Then `shadows` (acne: raise `depth_bias` or `slope_bias`; jagged edges: `pcf_radius`; shadows missing away from the centre: `extent_m`, or for a landscape `view_distance_m` so the map follows the camera), `ibl` intensity for how much the sky lights the shadows, `clouds`, and the sun with `environment_set`.
5. **Then materials.** `material_get` on the entities that matter, then `material_set` for roughness, metallic, colour, clearcoat, glass or emission — one entity at a time.
6. **One change per step.** After every `render_settings_set` or `material_set`, take another `screenshot` from the same camera and compare it with the previous one. Keep a change only if it moves toward the goal; otherwise `render_settings_reset` the group or `material_set` with `revert`.
7. **Finish** with a short list of the changes you kept and why. `scene_save` stores the render settings and material overrides with the scene — save only if asked.
