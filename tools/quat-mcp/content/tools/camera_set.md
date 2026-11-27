Move the viewport camera; returns the new camera (as `camera_get`). Usually give position plus `look_at`, e.g. `{"position": [12, 8, 12], "look_at": [0, 1, 0]}`. Alternatively set `yaw_deg` (0 = +X, 90 = +Z) and `pitch_deg` (positive = up). `fov_deg` is the vertical field of view.

This moves the user's view too. To look at something without disturbing the user, pass a `camera` to `screenshot` instead.

Clip planes: by default the engine derives them from the scene: the far plane reaches the farthest object and the whole terrain (streamed tiles included), and the near plane grows with it to keep depth precision. Set `near` / `far` in metres only for a special shot (e.g. `"near": 0.01` for an extreme close-up); `"auto"` returns a plane to the derived value.

`cut`: whether the move is a teleport. A cut restarts temporal anti-aliasing so the new view has no ghosting; continuous motion keeps it so a fly-through does not shimmer. When omitted, a jump of more than 10% of the far plane or a turn of more than 30 degrees counts as a cut. Pass `"cut": false` for each step of a smooth camera path.

`lens` sets the camera's physical lens (only the fields given change): `focal_length_mm` (replaces `fov_deg` when above 0), `aperture_f`, `focus_distance_m` (what is sharp when the `dof` render setting is on), `shutter_s` (motion-blur length; with `iso` and `aperture_f` also the exposure when `exposure.physical` is on) and `sensor_height_mm`. A shallow-focus portrait of something 3 m away: `{"lens": {"focal_length_mm": 85, "aperture_f": 1.8, "focus_distance_m": 3}}` plus `render_settings_set {"dof": {"enabled": true}}`.
