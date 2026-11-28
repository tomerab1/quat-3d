Move the viewport camera; returns the new camera (as `camera_get`). Usually give position plus `look_at`, e.g. `{"position": [12, 8, 12], "look_at": [0, 1, 0]}`. Alternatively set `yaw_deg` (0 = +X, 90 = +Z) and `pitch_deg` (positive = up). `fov_deg` is the vertical field of view.

This moves the user's view too. To look at something without disturbing the user, pass a `camera` to `screenshot` instead.

Clip planes: by default the engine derives them from the scene: the far plane reaches the farthest object and the whole terrain (streamed tiles included), and the near plane grows with it to keep depth precision. Set `near` / `far` in metres only for a special shot (e.g. `"near": 0.01` for an extreme close-up); `"auto"` returns a plane to the derived value.

`cut`: whether the move is a teleport. A cut restarts temporal anti-aliasing so the new view has no ghosting; continuous motion keeps it so a fly-through does not shimmer. When omitted, a jump of more than 10% of the far plane or a turn of more than 30 degrees counts as a cut. Pass `"cut": false` for each step of a smooth camera path.
