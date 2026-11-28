# Input actions (G5)

Gameplay reads **actions**, not keys. An action has a name (`jump`,
`camera.pan`), a type and a list of bindings to device controls. Rebinding,
gamepads and UI focus all change what drives an action without touching the
code that reads it. The design is in
[`docs/design/cozy-builder.md` §4.1](design/cozy-builder.md#41-input-actions-g5).
It follows the action / binding / interaction / processor split of Unity's
Input System and Unreal's Enhanced Input (see
[`reading/README.md`](reading/README.md#input)).

- C++: `engine/input/actions.hpp` (`input::Actions`).
- Lua: `input.action`, `input.axis`, `input.vector`, … in
  [`scripting.md`](scripting.md#input).
- Remote / MCP: `input.actions`, `input.inject`, `input.bind`, `input.context`
  in [`remote-control.md`](remote-control.md).

## Where maps come from

1. **The engine default map** holds G7's built-in names: `move_x`, `move_y`,
   `look_x`, `look_y` (axis1d) and `jump`, `fire`, `aim`, `interact`,
   `sprint`, `pause` (button), all in the always-active `default` context.
   Scripts written against G7 keep working.
2. **The project map**, `assets/input/actions.json`, is merged on top. An
   action or context with an existing name replaces it.
3. **The player's rebinding file** is applied last. It holds only the actions
   whose bindings differ from 1 + 2. It lives in SDL's per-user preferences
   directory (`input.json`), or at `QUAT_INPUT_USER_FILE`, where an empty
   value means no file. It is never inside the game data. A bad file is
   reported and ignored as a whole.

## Map format

```json
{
  "version": 1,
  "contexts": [
    {"name": "camera", "active": true},
    {"name": "build"},
    {"name": "menu", "exclusive": true}
  ],
  "actions": [
    {"name": "camera.pan", "type": "axis2d", "context": "camera",
     "bindings": [
       {"composite": "2d", "up": "key:w", "down": "key:s", "left": "key:a", "right": "key:d"},
       {"source": "gamepad:left_stick_xy", "modifiers": [{"dead_zone": 0.15}]},
       {"source": "edge", "edge_px": 8}
     ]},
    {"name": "camera.orbit_drag", "type": "axis1d", "context": "camera",
     "bindings": [{"source": "mouse:delta_x", "with": "mouse:right"}]},
    {"name": "camera.zoom", "type": "axis1d", "context": "camera", "bindings": ["mouse:wheel"]},
    {"name": "camera.grab", "type": "button", "context": "camera",
     "trigger": {"drag": 4}, "bindings": ["mouse:middle"]},
    {"name": "edit.undo", "type": "button", "trigger": "pressed", "bindings": ["key:ctrl+z"]},
    {"name": "edit.redo", "type": "button", "trigger": "pressed", "bindings": ["key:ctrl+shift+z"]}
  ]
}
```

Names use `a-z 0-9 _ . -` and are at most 64 characters long. A map holds at
most 512 actions, 32 contexts and 16 bindings per action. Parsing is strict:
an unknown field or a bad value is an error that names its path, e.g.
`actions[3].bindings[0]: unknown key 'hyperspace'`.

### Types

| Type | Value |
|---|---|
| `button` | `[0, 1]`: the strongest binding's magnitude (a trigger axis works too) |
| `axis1d` | a number (x) |
| `axis2d` | `[x, y]`: x is right, y is up / forward |

### Sources

| Source | Reads |
|---|---|
| `key:<name>`, `key:ctrl+z`, `key:ctrl+shift+z` | A key. Chord modifiers are `ctrl`, `shift` and `alt`, either side of the keyboard. |
| `mouse:left\|middle\|right\|x1\|x2` | A mouse button. |
| `mouse:delta`, `mouse:delta_x`, `mouse:delta_y` | Window coordinates moved this frame. y is down. |
| `mouse:wheel` | Wheel steps this frame. Positive is away from the user. |
| `pointer`, `pointer_x`, `pointer_y` | The pointer relative to the viewport, in window coordinates. y is down. |
| `edge`, `edge_x`, `edge_y` | The direction of the screen edge the pointer is in (top = up). |
| `gamepad:<button>` | `south`/`a`, `east`/`b`, `west`/`x`, `north`/`y`, `back`, `guide`, `start`, `left_stick`, `right_stick` (the stick clicks), `left_shoulder`, `right_shoulder`, `dpad_*`. |
| `gamepad:<axis>` | `left_x`, `left_y`, `right_x`, `right_y` (y down, as SDL reports), `left_trigger`, `right_trigger`. |
| `gamepad:left_stick_xy`, `gamepad:right_stick_xy` (also `_x` / `_y`) | A stick as 2D, with y up. |

Key names are the ones `input.key` takes in [`scripting.md`](scripting.md#input).

Pointer positions, deltas, `edge_px` and `drag` distances are in window
coordinates as SDL reports them: pixels on Windows and X11, points on a
high-density macOS / Wayland window (one point is two framebuffer pixels on a
Retina display), so they keep their physical size on every display. Rays
through the pointer depend only on its position relative to the viewport.

`edge` zones are `edge_px` wide (default 8). By default they work only while
the window is fullscreen and focused; set `"windowed": true` to use them in a
window. Corners are normalised, so a corner pan is no faster than an edge pan.

### Bindings

A binding is a source string, or an object:

- `{"source": "...", "modifiers": [...], "with": "<source>"}`. `with` names
  another control that must be held, e.g. mouse delta only while the right
  button is down.
- `{"composite": "1d", "negative": "key:q", "positive": "key:e"}`.
- `{"composite": "2d", "up": ..., "down": ..., "left": ..., "right": ...}`.
  Any part may be omitted. Parts must be 1D. Digital diagonals are
  normalised.

When several bindings fire at once, **the strongest one wins**. A stick and
keys do not add up past full deflection.

### Triggers

| Trigger | Active |
|---|---|
| `down` (default) | While actuated |
| `pressed` / `released` | The frame it goes down / up |
| `{"tap": 0.25}` | On release, if held at most that many seconds |
| `{"hold": 0.5}` | While held at least that many seconds |
| `{"double_click": 0.3}` | On a second press within that gap |
| `{"drag": 4}` | From the frame the pointer leaves a dead zone of that many pixels around the press point, until release. A click is not a drag. |

An action is *actuated* when its value's magnitude reaches `press_point`
(buttons, default 0.5) or is non-zero (axes). Lua's `action_pressed` and
`action_released` are the trigger's start and end edges.

### Modifiers

On a binding, and then again on the action:

| Modifier | Effect |
|---|---|
| `{"dead_zone": 0.15}` or `[lo, hi]` | Radial: the magnitude is remapped from [lo, hi] to [0, 1] and the direction is kept |
| `{"scale": 2}` or `[x, y]` | Multiplies the value |
| `"invert"`, `{"invert": "x" \| "y" \| "xy"}` | Flips axes |
| `{"smoothing": 0.05}` *(action only)* | Exponential smoothing with that half-life (s). Actuation is not delayed. |
| `"camera_relative"` *(axis2d action only)* | Rotates (right, forward) into world (x, z) for the view's heading |

## Contexts

The contexts form a stack, bottom to top. Contexts marked `active` start on
it, and scripts or remote calls push and pop them. Only actions in contexts on
the stack respond. Evaluation runs from the top down:

- A **consuming** context (the default) hides the controls its actuated
  actions read from the contexts below it. A build tool's `mouse:left` then
  never also fires the camera's.
- An **exclusive** context hides everything below it, as a modal menu does.
- A **chord beats the plain key**: while ctrl+z fires, a binding on z alone
  reads released. A chord with more modifiers beats one with fewer.
- **UI focus.** Over a UI panel, pointer controls cannot start an action, so
  clicks on the editor's panels (and, later, G6 widgets) never reach the
  world. An action that is already held keeps the pointer (mouse capture), so
  a drag that leaves the viewport continues. While a text field owns the
  keyboard, keys read released.

## Frames and the simulation step

`Actions::update` runs once per frame, after the device is sampled.
Per-frame consumers read `frame()`: the camera rig, and later the UI.
Fixed-step consumers call `take_sim_sample()` once per step. That sample
carries every start and end edge since the previous take, so a press is seen
by **exactly one** step, even when a frame runs several steps or none (a
paused or slow frame). Scripts' `on_fixed_update` gets a second latch inside
the script runtime.

The evaluation is deterministic. The result depends only on the map, the
context stack, the sequence of (device state, dt) and injections: there are
no clocks and no randomness. A recorded input stream therefore replays
exactly (`tests/input/actions_test.cpp`, `Deterministic`).

## Injection

`Actions::inject(name, value, frames)` and remote `input.inject` make an
action read a value for N frames. The value goes after the action's bindings
and modifiers and before its trigger, so a `pressed` or `drag` action
behaves as it would for a real press. Tests and MCP playtests use this to
drive gameplay without a device.
