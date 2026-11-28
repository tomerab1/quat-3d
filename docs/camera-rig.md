# Camera rig (G15.1)

The camera rig is a generic orbit / strategy camera: the camera of town
builders, RTS games and model viewers. It is a `CameraRig` component that
drives its entity's `Transform`. A `Camera` on the same entity stays the
lens. Keep the entity a root, because the rig writes its local transform.
The design is in
[`docs/design/cozy-builder.md` §4.2](design/cozy-builder.md#42-camera-rig-g151-orbit--strategy-rig).

```json
"components": {
  "camera": {"fov_deg": 50},
  "camera_rig": {"focus": [0, 0, 0], "yaw_deg": 45, "distance": 40,
                 "pitch_near_deg": -40, "pitch_far_deg": -65,
                 "min_distance": 6, "max_distance": 180,
                 "bounds": {"shape": "rect", "min": [-100, -100], "max": [100, 100], "margin": 20}},
  "camera_rig_input": {"pan": "camera.pan", "orbit": "camera.orbit",
                       "orbit_delta": "camera.orbit_drag", "zoom": "camera.zoom",
                       "drag_pan": "camera.grab"}
}
```

## How it moves

The rig is target-space. The authored fields are **targets**: `focus`,
`yaw_deg`, `distance`, and `pitch_deg` when pitch does not follow the zoom.
The rig's damped state chases them. Each degree of freedom uses the exact
critically damped spring in half-life form (`engine/core/spring.hpp`; Holden,
*Spring-It-On*, 2021; Lowe, GPG4 2004). The spring is frame-rate
independent and never overshoots. Yaw takes the shortest arc. Distance is
damped on a **log** scale, so a zoom step feels the same at 6 m and at 180 m.

- **Pitch eases with zoom** (`pitch_follows_zoom`, the default). Pitch is
  interpolated between `pitch_near_deg` at `min_distance` and
  `pitch_far_deg` at `max_distance` on log distance: a three-quarter view
  close up, nearly top-down far away (Nesky, GDC 2014; Haigh-Hutchinson,
  2009).
- **Yaw** is free, or snaps in steps of `yaw_step_deg`. With steps, a key
  press turns one step, and a mouse drag steps each time it has turned half
  a step.
- **Pan** moves the focus in the yaw frame, at `pan_speed` × distance per
  second. Keys, a stick and screen-edge zones all feed it.
- **Grab-the-world drag pan.** At drag start the rig casts the pointer onto
  the ground: physics first, then the plane through the focus. Each frame the
  focus moves so the grabbed point stays under the pointer, 1:1, without
  smoothing. On release the drag velocity carries on and decays with
  `drag_inertia_halflife_s`.
- **Zoom toward the cursor.** The distance scales by `zoom_step^-steps`, and
  the focus shifts so the ground point under the cursor stays there once the
  rig settles. The shift is exact at any pitch: translating the whole camera
  translates the cursor ray's hit on a horizontal plane by the same amount,
  so the solve is one ray cast and a subtraction
  (`rig_focus_keeping_point`).
- **Ground.** With `follow_ground`, the focus height follows the ground: the
  mean of five samples around the focus, smoothed by
  `focus_ground_halflife_s`. The camera stays `ground_clearance` above the
  ground under itself by **looking further down**, never by moving the focus
  (`rig_clear_pitch`).
- **Bounds.** `rect` or `circle` on XZ. While dragging, the focus may stretch
  up to `margin` past the bounds on a rubber band. Otherwise the target is
  clamped, so after a drag the focus springs back inside.
- **Framing.** `camera_rig_frame()` and remote `camera.rig.frame` reuse
  `frame_box()` (`camera_framing.hpp`) at the rig's current heading. Pitch
  and distance are solved together when pitch follows the zoom.

- **Lens (R20).** Pointer rays and framing use the `Camera`'s effective
  field of view, so a focal length set on the lens is respected. With
  `focus_lens` (the default), the lens's `focus_distance_m` follows the
  rig's damped distance, so depth of field, including a tilt-shift look,
  stays focused on what the rig orbits.

The rig runs on **real** time, before the scene tick, so the camera still
moves while the simulation is paused.

## Input

`CameraRigInput` names the G5 actions that drive the rig ([`input.md`](input.md)):

| Field | Action | Meaning |
|---|---|---|
| `pan` | axis2d, rate | x right, y forward; `pan_speed` × distance per second |
| `orbit` | axis1d, rate | Positive turns right; `orbit_speed_deg` per second |
| `orbit_delta` | axis1d, per frame | e.g. `mouse:delta_x` with `"with": "mouse:right"`; `orbit_delta_deg` per unit |
| `pitch_delta` | axis1d, per frame | Free pitch only |
| `zoom` | axis1d, per frame | Wheel steps; positive zooms in |
| `drag_pan` | button with a `drag` trigger | Grab the world |

A game that wants other controls leaves `camera_rig_input` off and writes the
rig's targets itself, from C++, from Lua (`entity:set("camera_rig", {...})`)
or remotely.

The rig only takes input while the game view is live: always in a non-editor
build, and in the editor during Play through the game camera. The editor's
fly camera and a rig therefore never both take WASD.

## Remote and MCP

- `camera.rig.get {entity?}` returns the targets and `current` (the damped
  pose, including `eye`).
- `camera.rig.set {entity?, immediate?, ...camera_rig fields}` moves the
  targets. `immediate` snaps the damped state and places the entity at once,
  so a following screenshot comes from an exact, repeatable pose.
- `camera.rig.frame {entity?, target?, margin?, immediate?}` frames an entity
  or the scene.

The quat-mcp tools `camera_rig_get`, `camera_rig_set` and `camera_rig_frame`
call these methods. Without `entity`, the methods use the rig on the active
camera, or the only rig in the scene.

## Not in G15.1

- Collision against buildings by sphere cast (A10.1). It is rarely needed
  above 40° pitch.
- G15.2 (third-person follow rig) and G15.3 (cutscene camera tracks).
