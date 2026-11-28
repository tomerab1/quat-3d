# Gameplay scripting (Lua)

quat-3d runs gameplay code written in **Lua 5.4**. A script is a `.lua` file
under `assets/scripts/`; an entity runs it through its `script` component.
Scripts run while the game plays (Play mode: the editor's Play button,
`sim.play`, `QUAT_PLAY=1`, or a `QUAT_SCENE` game in the non-editor build)
and reload when their file changes. This page is the full
reference of what a script can do; AI assistants read it through quat-mcp
(`quat://docs/scripting`).

## A first script

```lua
-- assets/scripts/spinner.lua
local Spinner = {
    properties = { speed = 90 }, -- degrees per second, editable per entity
}

function Spinner:on_start()
    print("spinning", self.entity:name())
end

function Spinner:on_update(dt)
    self.entity:rotate(quat.angle_axis(self.speed * dt, vec3(0, 1, 0)))
end

return Spinner
```

Attach it: in the editor, *Add component → Script* and pick the file; over
the remote API / MCP, `scene.update {entity, components: {script: {path:
"spinner.lua"}}}`; in a scene file, `"script": {"path": "spinner.lua"}`.

## Script files

- A script returns a table of callbacks (`return Spinner`). A script that
  returns nothing works too: its global functions are the callbacks
  (`function on_update(self, dt) ... end`).
- Each file has its own globals; one file runs once per play session and all
  entities using it share that module (top-level `local`s are shared too).
- Each entity gets its own instance table, `self`. `self.entity` is the
  entity; declared properties are fields of `self`; anything else you store
  on `self` is per-entity state. Callbacks not defined on `self` are looked up
  on the script's table.
- Paths are relative to `assets/scripts/` and use `/`: `"coin_collector/coin.lua"`.
- `require("lib.util")` (or `"lib/util"`) loads `assets/scripts/lib/util.lua`
  once and returns what it returns. Nothing outside `assets/scripts/` can be
  loaded.

## Callbacks

| Callback | When |
|---|---|
| `on_start(self)` | Once, before the instance's first update (the frame after the entity got its script). |
| `on_fixed_update(self, dt)` | At a fixed 60 Hz (`dt` = 1/60), before `on_update`; up to 5 per frame. |
| `on_update(self, dt)` | Every simulated frame; `dt` is the simulation time step (seconds). |
| `on_trigger_enter(self, other)` | A sensor overlap began: either this entity's sensor collider was entered by `other`, or this entity entered `other`'s sensor. |
| `on_trigger_exit(self, other)` | That overlap ended. |
| `on_destroy(self)` | The instance ends: the entity was destroyed, its script removed, disabled or pointed at another file, or the file was hot-reloaded. |

Order within a frame: physics steps, trigger callbacks fire, new instances
start (`on_start`), `on_fixed_update`, `on_update`, then the destroys
requested during the frame happen. Movement commands a script gives
(`move`, impulses) take effect at the next physics step.

While the simulation clock is paused (`debug_pause`), scripts are paused too;
`debug_step` advances them one frame per step.

Characters (`character` component) take part in triggers: walking a
character into a sensor fires both entities' callbacks.

## Errors

A callback that raises an error stops that entity's script (the others keep
running). The error, with the file and line (`coin.lua:12: attempt to index
a nil value`) and a short traceback, goes to the engine log (category
`script`, level `error`; `debug_log`) and to the script's status (editor
*Stats → Scripts*, `script.list`). Fixing and saving the file restarts the
stopped instances.

Engine calls report misuse the same way: `e:position()` on a destroyed
entity raises `stale.lua:14: entity 42 no longer exists`; `e:set("collider",
{radius = 2})` raises the codec's message (`collider.radius: unknown field
(known: shape, half_extents, offset, sensor)`). Use `pcall` to handle an
error yourself.

## Limits and sandbox

- Each callback (and each file's first run) may execute at most **5 million
  Lua instructions** and grow the heap by at most **64 MiB**; all scripts
  together use at most **256 MiB**. A script past a limit is stopped with
  `instruction limit exceeded (…): an endless loop?` or `memory limit
  exceeded`; `pcall` cannot catch these.
- Available: `assert error ipairs next pairs pcall xpcall select tonumber
  tostring type rawequal rawget rawset rawlen setmetatable getmetatable
  require print _VERSION` and the `string`, `table`, `math`, `utf8`,
  `coroutine` libraries. **Not available:** `io`, `os`, `package`, `debug`,
  `load`, `loadfile`, `dofile`, `collectgarbage`. Engine and library tables are
  read-only (`rawset` on them fails too); `getmetatable` returns nothing for
  engine values; `setmetatable` refuses a metatable with `__gc` (finalizers
  would run outside the limits — clean up in `on_destroy`). An error object
  that is not a string or number is reported by its type, without calling its
  `__tostring`.
- Use `time` instead of `os.clock`, and the log instead of `io.write`.

## Math: `vec3`, `quat`

```lua
local v = vec3(1, 2, 3)      -- also vec3() == (0,0,0), vec3(s) == (s,s,s)
v.x = 4                      -- fields x, y, z
local w = v * 2 + vec3(1) - v / 4   -- + - with vec3; * / with numbers (or vec3, per component)
-v, v == w, tostring(v)      -- "vec3(4, 2, 3)"
v:length()  v:length_squared()  v:normalized()  v:dot(w)  v:cross(w)
v:distance(w)  v:lerp(w, 0.5)  v:copy()

local q = quat()                         -- identity; quat(x, y, z, w)
quat.euler(vec3(pitch, yaw, roll))       -- degrees; yaw about Y, then pitch about X, then roll about Z
quat.angle_axis(degrees, vec3(0, 1, 0))
quat.look_rotation(forward, up?)         -- -Z looks along `forward`
q * q2, q * v                            -- compose, rotate a vector (also q:rotate(v))
q:to_euler()  q:inverse()  q:normalized()  q:slerp(q2, t)
q.x, q.y, q.z, q.w
```

Conventions: Y is up, distances are metres, angles are degrees. An entity's
forward is **-Z** (`e:forward()`), right is +X, up is +Y.

`vec3` / `quat` values are copies: changing `p` after `local p =
e:position()` does not move the entity — call `e:set_position(p)`.

## Entity

Entities are handles (`Entity`). `self.entity` is the script's own; others
come from `scene.find`, `scene.spawn`, trigger callbacks, `e:parent()` and
so on. Two handles to the same entity are `==`. A handle outlives its entity:
calls on a destroyed entity raise an error, except `valid()`.

### Identity and hierarchy

| Call | Returns / does |
|---|---|
| `e.id` | Integer id (the id remote tools / MCP use). |
| `e:valid()` | False once the entity is destroyed. |
| `e:name()`, `e:set_name(s)` | Name (may be empty). |
| `e:parent()` | Parent entity or `nil`. |
| `e:set_parent(p)` | Re-parent under `p` (`nil` = root), keeping the world pose. Refuses cycles. |
| `e:children()` | Array of child entities. |
| `e:destroy()` | Destroy `e` and its children at the end of the current phase (`on_destroy` runs first). The handle stays valid until then. |
| `e:script()` | The `self` table of `e`'s running script, or `nil` — call its functions to talk to other scripts: `scene.find("Game"):script():collect(1)`. |

### Transform

| Call | Notes |
|---|---|
| `e:position()`, `e:set_position(v)` | World position. Setting it also moves the entity's physics body / character controller (a teleport). |
| `e:local_position()`, `e:set_local_position(v)` | Relative to the parent. |
| `e:translate(v)` | Move by `v` in world space. |
| `e:rotation()`, `e:set_rotation(q)` | World rotation (`quat`). |
| `e:rotate(q)` | Apply `q` on top of the current world rotation. |
| `e:look_at(target, up?)` | Turn so forward (-Z) points at the world point `target`. |
| `e:scale()`, `e:set_scale(v or number)` | Local scale. Physics shapes keep the scale they were created with. |
| `e:forward()`, `e:right()`, `e:up()` | World axes (-Z, +X, +Y rotated). |

Children follow their parent immediately.

### Components

Every component of the engine's codec can be read and written as a Lua
table: `transform`, `primitive`, `instanced_mesh`, `scatter`, `point_light`,
`dir_light`, `camera`, `collider`, `rigidbody`, `character`, `nav_agent`,
`behavior`, `patrol`, `script`, `terrain`, `net_replicated`, `audio_source`,
`audio_listener`, `animator` (edit only) and `mesh` (read only). Field names
and values are exactly those of the remote API / MCP component reference
(`scene.components`, docs/remote-control.md).

| Call | Notes |
|---|---|
| `e:get(name)` | The component as a table, or `nil` when the entity lacks it. Arrays of three numbers become `vec3` (positions, colours). |
| `e:set(name, fields)` | Add the component or merge `fields` into it. Unknown fields and bad values raise the codec's error and change nothing. `vec3` values are accepted wherever `[x, y, z]` is. |
| `e:has(name)` | True if the entity carries it. |
| `e:remove(name)` | Remove it (`transform` cannot be removed); returns whether it was there. |

```lua
e:set("point_light", { color = vec3(1, 0.6, 0.2), radius = 6, intensity = 4 })
e:set("primitive", { color = "#ff0000" })        -- recolour
local body = e:get("rigidbody")                   -- { motion = "dynamic", mass = 1.0 }
```

Prefer the direct calls (transform, physics below) every frame; `get`/`set`
go through JSON and suit occasional changes.

### Physics

Physics exists while playing. Bodies come from `collider` + `rigidbody`
(a collider without a rigidbody becomes a static body on Play); a sensor is a
collider with `sensor = true`.

| Call | Notes |
|---|---|
| `e:add_impulse(v)` | Push a **dynamic** body (N·s). Works right after `scene.spawn` too. |
| `e:set_velocity(v)` | Set a dynamic body's linear velocity (m/s). |
| `e:velocity()` | A dynamic body's or a character's velocity. |
| `e:move(dir)` | Character controller: walk direction (x/z, length ≤ 1, scaled by the character's `speed`). Hold it every frame; `vec3(0)` stops. |
| `e:jump(speed)` | Character: launch upwards at `speed` m/s if on the ground; returns whether it jumped. |
| `e:on_ground()` | Character: standing on something. |

### Navigation and audio

| Call | Notes |
|---|---|
| `e:set_nav_target(v)` | Send a `nav_agent` to the world point `v` (a navmesh is baked on Play when needed). |
| `e:stop_nav()` | Hold the agent where it stands. |
| `e:nav_arrived()` | The agent reached its target. |
| `e:play_sound()`, `e:stop_sound()` | Play / stop the entity's `audio_source` clip. |

## `scene`

| Call | Returns / does |
|---|---|
| `scene.find(name)` | The first entity with that exact name, or `nil`. Cache it in `on_start`. |
| `scene.find_all(name)` | Array of every entity with that name. |
| `scene.with_component(name)` | Array of every entity carrying that component. |
| `scene.spawn(spec)` | Create an entity. `spec` keys: `name`, `parent` (Entity), and any component (`transform` is local to the parent). Fails without leaving anything behind when a component is wrong. |
| `scene.spawn_model(path, spec?)` | Instantiate a `.gltf` / `.glb` (path relative to the project root, e.g. `"assets/meshes/crate.glb"`); several root nodes are grouped under one entity. `spec` as for `spawn`. Returns the root. |
| `scene.clone(e)` | Copy `e` and its children (a prefab-style duplicate), as a sibling. Physics bodies, sounds and scripts of the copy start fresh. |
| `scene.destroy(e)` | Same as `e:destroy()`. |
| `scene.count()` | Number of entities. |

```lua
local coin = scene.spawn({
    name = "Coin",
    transform = { position = vec3(3, 1, 0), scale = vec3(0.6, 0.08, 0.6) },
    primitive = { shape = "cylinder", color = "#ffc832", metallic = 1 },
    collider = { shape = "sphere", half_extents = vec3(1.5), sensor = true },
    rigidbody = { motion = "kinematic" },
    script = { path = "coin_collector/coin.lua", properties = { value = 5 } },
})
```

## `physics`

`physics.raycast(origin, direction, max_distance?, include_sensors?)` returns
the closest hit as `{ entity, point, normal, distance, sensor }` (`entity` is
`nil` for terrain and other unowned bodies) or `nil`. `max_distance`
defaults to 1000 m; sensors are skipped unless `include_sensors` is true.

## `input`

Keyboard, mouse and the first gamepad, sampled once per frame, plus the
named **actions** of the project's action map (G5, [`input.md`](input.md)).
While an editor text field has focus, keys and key-bound actions read as
released.

| Call | Notes |
|---|---|
| `input.key(name)`, `input.key_pressed(name)`, `input.key_released(name)` | Held now; went down this frame; went up this frame. |
| `input.mouse_button(b)`, `input.mouse_pressed(b)`, `input.mouse_released(b)` | `b` = `"left"`, `"middle"`, `"right"`, `"x1"`, `"x2"`. |
| `input.mouse_position()` | `x, y` in window pixels. |
| `input.mouse_delta()` | `dx, dy` moved this frame. |
| `input.mouse_wheel()` | Wheel steps this frame. |
| `input.gamepad_connected()` | A gamepad is connected. |
| `input.gamepad_button(b)`, `input.gamepad_pressed(b)`, `input.gamepad_released(b)` | `south`/`a`, `east`/`b`, `west`/`x`, `north`/`y`, `back`, `guide`, `start`, `left_stick`, `right_stick`, `left_shoulder`, `right_shoulder`, `dpad_up`, `dpad_down`, `dpad_left`, `dpad_right`. |
| `input.gamepad_axis(a)` | `left_x`, `left_y`, `right_x`, `right_y` in [-1, 1] (y down positive), `left_trigger`, `right_trigger` in [0, 1]. |
| `input.axis(name)` (alias `input.value`) | The value of a button / axis1d **action** ([input actions](input.md)). The default map has `move_x` (A/D, ←/→, left stick), `move_y` (W/S, ↑/↓, left stick; forward positive), `look_x`, `look_y` (right stick; up positive), in [-1, 1]. |
| `input.vector(name)` | An axis2d action as `vec3(x, y, 0)` (y up / forward). |
| `input.move_vector()` | `vec3(move_x, 0, -move_y)` clamped to length 1: a world-space walk direction (forward = -Z) ready for `e:move`. |
| `input.action(name)`, `input.action_pressed(name)`, `input.action_released(name)` | An action is active / its trigger started / ended. Defaults: `jump` (Space, gamepad A), `fire` (Ctrl, left mouse, right trigger), `aim` (right mouse, left trigger), `interact` (E, gamepad X), `sprint` (Shift, left stick click), `pause` (P, Start); a project adds its own in `assets/input/actions.json`. In `on_fixed_update` each press is seen by exactly one fixed step. |
| `input.held_time(name)` | Seconds the action has been held (0 when released). |
| `input.push_context(name)`, `input.pop_context(name)` | Push / pop an input context (applies from the next frame); `pop_context` returns whether it was on the stack. |

Key names: `a`–`z`, `0`–`9`, `f1`–`f12`, `space`, `enter` (`return`),
`escape`, `tab`, `backspace`, `up`, `down`, `left`, `right`, `left_shift`,
`right_shift`, `left_ctrl`, `right_ctrl`, `left_alt`, `right_alt`, `insert`,
`delete`, `home`, `end`, `page_up`, `page_down`, `minus`, `equals`,
`left_bracket`, `right_bracket`, `backslash`, `semicolon`, `apostrophe`,
`grave`, `comma`, `period`, `slash`, `caps_lock`. Unknown names raise an
error listing what exists.

## `time`, `log`, `print`

| Call | Returns |
|---|---|
| `time.delta()` | This frame's simulation step (the `dt` of `on_update`). |
| `time.elapsed()` | Simulated seconds since Play started. |
| `time.frame()` | Simulated frames since Play started. |
| `time.fixed_delta()` | The fixed step of `on_fixed_update` (1/60). |
| `time.clock()` | The scene's world clock and sky as a table: `datetime`, `latitude_deg`, `longitude_deg`, `time_scale`, `enabled`, …, `sun` / `moon` (`azimuth_deg` compass bearing, `elevation_deg`, `direction` vec3, `illuminated_fraction`, `phase`, sunrise / sunset / moonrise / moonset times). The same fields as the remote `time.get` ([`remote-control.md`](remote-control.md#world-clock-roadmap-r221)); `in_scene` is false while the scene has no clock. |
| `time.set_clock{...}` | Change the clock (creating it if needed): any of `datetime`, `local_time` (`"hh:mm"`), `utc_offset_h`, `latitude_deg`, `longitude_deg`, `north_offset_deg`, `time_scale`, `fixed_day`, `enabled`, `drive_sun`, `sun_intensity`. Unknown or out-of-range fields raise an error; nothing changes then. Returns `time.clock()`. |
| `time.skip(hours)` | Move the clock by `hours` (negative goes back, at most ten years). Raises an error without a clock. Returns `time.clock()`. |
| `print(...)` | Log at level `info`, category `script`, prefixed with the script path. |
| `log.debug(...)`, `log.info(...)`, `log.warn(...)`, `log.error(...)` | Log at that level. |

Arguments are converted with `tostring` and separated by tabs, as Lua's
`print` does. Read the log with quat-mcp's `debug_log` (`category:
"script"`).

## Properties

A script declares tunable values in its `properties` table (numbers,
booleans, strings and `vec3`s). Each entity's `script` component can
override them:

```json
"script": { "path": "coin_collector/coin.lua", "properties": { "value": 5 } }
```

The editor's inspector shows the declared properties with their defaults
and edits the overrides; `scene.update` merges `properties` per name (`null`
removes an override). Values arrive on `self` before `on_start`; an edit
while playing updates `self` on the next frame. Top-level script code runs
when the inspector describes the file, so keep it free of scene access (do
that in `on_start`).

## Hot reload

Saving a script (or `script.update` over the remote API) recompiles it while
the game runs: every instance gets `on_destroy`, a fresh `self` and
`on_start` again. A compile or load error keeps the previous version running
and shows the error (with its line) in the log and the script's status.
Editing a module loaded with `require` reloads the scripts that use it.
Editor builds watch `assets/scripts/` automatically
(`QUAT_SCRIPT_HOT_RELOAD=0` turns it off, `=1` turns it on in the non-editor
game).

## The Coin Collector demo

`assets/scenes/coin_collector.json` with `assets/scripts/coin_collector/`:
a capsule player (WASD / arrows / left stick, Space / A to jump) collects a
ring of spinning coins; the score and a win message go to the log.

```sh
QUAT_SCENE=assets/scenes/coin_collector.json QUAT_PLAY=1 ./build/release/game
```

In the editor, *File → Load* the scene, press *Play* and tick *Game camera*
to look through the follow camera. Over MCP: `scene_load`, `sim_play`, then
`debug_log` with `category: "script"`.
