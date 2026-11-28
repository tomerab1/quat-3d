---
name: tune-look
description: Tune how a running quat-3d scene looks - exposure, tonemapping, bloom, shadows, ambient occlusion, sky light, clouds, sun, camera lens, depth of field, motion blur, colour grading and per-entity materials - one change at a time, judged by screenshots from a fixed camera. Use when the user says the scene is too dark, washed out, flat, noisy or wants a mood such as golden hour or overcast.
compatibility: Needs the quat-mcp MCP server connected to a running quat-3d engine (started with QUAT_REMOTE).
metadata:
  quat-mcp-tools: "camera_frame camera_set screenshot render_settings_get render_settings_schema render_settings_set render_settings_reset environment_get environment_set scene_update material_get material_set debug_stats scene_save"
---

# Tune the look

Work like a colourist: look, change one thing, look again. Wrong exposure
makes every other judgement misleading, so it always comes first.

## Workflow

1. **Fix the view.** `camera_frame` (or `camera_set` with a `position` and
   `look_at`) to a representative view the user agrees with. Tuning compares
   images, so every later `screenshot` uses this same viewport camera (no
   `camera` argument): auto exposure adapts to the viewport, and a shot from
   elsewhere is exposed for the wrong view.
2. **Look first.** `screenshot` and describe what you see: overall
   brightness, clipped highlights or crushed shadows, glow, shadow quality,
   sky, how the materials read.
3. **Read the state.** `render_settings_get` for the current values,
   `render_settings_schema` once for every field's range, unit and default,
   `environment_get` for the sun and clouds.
4. **Exposure first.** With `auto` on, adjust `compensation_ev` in steps of
   0.3 to 1 EV. Touch `key`, `min_exposure` or `max_exposure` only if auto
   exposure pumps or overreacts. Turn `auto` off for a fixed, repeatable
   exposure (exposure = 2^`compensation_ev`).
5. **Then tonemap and highlights.** Pick the `tonemap` `operator`: `aces`
   (contrasty, saturated), `agx` (natural roll-off of bright colours toward
   white, the cinematic choice) or `pbr_neutral` (true base colours, for
   product shots); highlight roll-off then follows from exposure. Tune
   `bloom` (`threshold`, `intensity`, `radius`) until highlights glow
   without a haze over the image.
6. **Then shadows and ambient light.**
   - `shadows`: acne or stripes - raise `normal_bias_texels` or
     `depth_bias_texels`; jagged edges - `pcf_radius` or `sun_size_deg`;
     shadows ending too close - `max_distance`.
   - `ao` (`radius`, `strength`) and `contact_shadows` for small-scale
     contact.
   - `ibl` `intensity` for how much the sky lights the shadows.
   - `ssr` for reflections on glossy surfaces.
   - For a photographic look, light the scene with an HDRI panorama
     (`environment_set` with `sky: "hdri"` and `hdri.path`), then turn it
     with `hdri.rotation_deg` until its sun lights the subject the way you
     want.
   - The sun and clouds with `environment_set`: `elevation_deg` 5..10 is
     golden hour, 45..70 daylight; `clouds` `coverage` 0.85 is overcast.
7. **Then the camera and grade.** For a filmic shot, give the active
   camera a lens with `scene_update` on its `camera` component
   (`focal_length_mm`, `aperture_f`, `focus_distance_m` on the subject;
   `camera_set` `lens` for the editor view) and turn on `dof`. Motion
   blur is off by default; with `motion_blur` on, its length follows the
   camera's `shutter_s` (1/120 s is a 180-degree shutter at 60 fps). Turn
   `exposure` `physical` on only when the lights are physically scaled
   (sun around 100 000 lux): with the default lights it makes the frame
   almost black, so otherwise keep auto or manual exposure and use the lens
   for depth of field and motion blur only.
   Grade last: `grading` (`temperature_k` for warmth, `contrast`,
   `saturation`, per-channel `lift_*` / `gamma_*` / `gain_*`, a `.cube`
   look `lut` inside the project) and `film` (`vignette_natural`,
   `vignette_intensity`, subtle `grain_intensity`,
   `chromatic_aberration`).
8. **Then materials.** `material_get` on the entities that matter, then
   `material_set` (roughness, metallic, colour, clearcoat, glass, emission),
   one entity at a time. The author-material-shader skill covers materials
   in depth.
9. **Finish** with a list of the changes you kept and why. `scene_save`
   stores the render settings and material overrides with the scene; save
   only if asked.

## One change per step

After every `render_settings_set` or `material_set`, take another
`screenshot` and compare it with the previous one. Keep a change only if it
moves toward the goal; otherwise undo it with `render_settings_reset` for the
group or `material_set` with `revert`. Change one group per call: the engine
rejects the whole call when one field is out of range, and the error names
it.

## Verify

- The final screenshot and the first one side by side show the requested
  change and nothing else got worse (blown sky, black shadows, haze).
- `debug_stats`: frame time (`frame_time_ms` avg and worst) is not much
  higher than before. AO, shadow cascades and `max_distance` cost GPU time;
  `{"upscaler": {"mode": "performance"}}` renders at half resolution per axis
  and upscales (needs `taa` enabled) when frame time is the problem.

## Pitfalls

- Brightness problems are almost never fixed by bloom, IBL or materials:
  correct exposure first, then revisit.
- `debug_cascades` in `shadows` tints the cascades for diagnosis; turn it
  off before the final screenshot.
- `material_set` changes only that entity (the shared material asset is
  untouched); a look that should apply to many entities needs one call each
  or a change at the source asset.
- Emission above 1 glows only through bloom: a glowing sign that does not
  glow usually means bloom is off or its `threshold` is too high.
