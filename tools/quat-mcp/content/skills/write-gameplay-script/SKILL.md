---
name: write-gameplay-script
description: Write gameplay for a running quat-3d game as Lua scripts - pickups, doors, spinners, spawners, score keeping, character control - compiled before saving, attached with the script component, run in play mode and debugged from the script log. Use when asked to add behaviour, interaction, game rules or controls to a quat-3d scene with code.
compatibility: Needs the quat-mcp MCP server connected to a running quat-3d engine (started with QUAT_REMOTE) built with Lua scripting.
metadata:
  quat-mcp-tools: "scene_list scene_get scene_update scene_batch script_list script_get script_check script_create script_update script_delete scene_remove_components sim_play sim_stop debug_log debug_watch debug_pause debug_step debug_resume screenshot scene_save"
---

# Write a gameplay script

Work in small, verified steps: read the API, write, compile, attach, play,
read the log, fix one thing at a time.

## Workflow

1. **Learn the API.** Read the MCP resource `quat://docs/scripting`: the
   callbacks (`on_start`, `on_update`, `on_fixed_update`,
   `on_trigger_enter`, `on_trigger_exit`, `on_destroy`), the entity, scene,
   physics, input, time and log calls, the math types, the limits and the
   sandbox. Use only what it lists; when unsure, read it again instead of
   guessing.
2. **Look at the scene.** `scene_list` and `scene_get` for the entities
   involved (names, components, positions). `script_list` and `script_get`
   for scripts you can reuse or follow (the Coin Collector demo under
   `coin_collector/` is a complete example).
3. **Write the script.** It returns a table of callbacks:

   ```lua
   local Door = { properties = { open_height = 3, speed = 2 } }
   function Door:on_start()
       self.closed_y = self.entity:position().y
       self.opening = false
   end
   function Door:on_trigger_enter(other)
       if other:name() == "Player" then self.opening = true end
   end
   function Door:on_update(dt)
       if not self.opening then return end
       local p = self.entity:position()
       local target = self.closed_y + self.open_height
       if p.y < target then
           self.entity:set_position(vec3(p.x, math.min(target, p.y + self.speed * dt), p.z))
       end
   end
   return Door
   ```

   - Tunable values go in `properties`; they arrive on `self` before
     `on_start` and can be overridden per entity.
   - Cache `scene.find` results in `on_start`; keep per-entity state on
     `self`.
   - Check drafts with `script_check`, then `script_create` for a new file
     or `script_update` (after `script_get`) to replace one. A syntax error
     names the line: fix it and call again.
4. **Set up the entities.** Attach with `scene_update`:
   `{"entity": "Door", "components": {"script": {"path": "door.lua", "properties": {"speed": 3}}}}`
   (or `scene_batch` for many entities).
   - Physics needs a `collider`, plus a `rigidbody` for moving bodies.
   - Triggers are colliders with `sensor: true`; the entering object needs
     a body or a `character`.
   - Walking characters need a `character` component (`e:move(dir)` each
     frame).
5. **Run it.** `sim_play`, then `debug_log` with
   `{"category": "script"}`: `print` and `log.*` output and errors with
   `file:line` land there. Keep the returned `cursor` and pass it as
   `since_seq` next time to read only new lines. `script_list` shows which
   scripts stopped (`failed`, last `message`).
6. **Iterate.** `script_update` hot-reloads while playing: every instance
   gets `on_destroy`, a fresh `self` and `on_start` again. Fix one problem
   at a time and re-read the log after each change.
7. **Finish.** `sim_stop` restores the scene as it was before Play (what
   scripts spawned or moved disappears; the script components stay).
   `scene_save` keeps the setup if asked. `script_delete` the drafts you
   wrote and no longer use (it reports `entities_still_using`: detach those
   first with `scene_remove_components`). Summarise what the script does,
   its properties, and how to play it (controls).

## Verify

- `script_list`: the script's `status.state` is `ok` and `failed` is 0
  after playing.
- `debug_watch` the scripted entities while playing (or with
  `debug_pause` and `debug_step`, then `debug_resume`) and see the values
  change as intended; `screenshot` shows the effect.
- The script log holds no errors since your last change.

## Pitfalls

- Callbacks are methods: define them with `function Name:on_update(dt)`
  and call entity methods with a colon (`self.entity:position()`).
- `e:get` / `e:set` go through JSON; per-frame code should use the direct
  calls (`position`, `set_position`, `add_impulse`, `move`, ...).
- Physics calls (`add_impulse`, `set_velocity`, `physics.raycast`) work only
  while playing, on dynamic bodies or characters as documented.
- You cannot press keys or move the mouse over MCP. Input-driven code
  (`input.action`, `input.move_vector`) is verified by the user playing;
  test the logic behind it by temporarily driving it from a property or a
  timer, then restore the input code.
- Scripts run under an instruction and memory budget; an endless loop is
  stopped and reported as an error, not a hang.
- Edits to entities while playing are reverted by `sim_stop`; attach
  scripts and set properties in edit mode to keep them.
