Create a new Lua gameplay script file under `assets/scripts/` and return `{path, bytes, entities, running?, status?}`. The source is compiled first: a syntax error names the line (`syntax error at line 3: coin.lua:3: ...`) and nothing is written. Fails if the file exists (use `script_update`).

A script returns a table of callbacks; `properties` are per-entity tunables:

```lua
local Spinner = { properties = { speed = 90 } }
function Spinner:on_update(dt)
    self.entity:rotate(quat.angle_axis(self.speed * dt, vec3(0, 1, 0)))
end
return Spinner
```

Read the `quat://docs/scripting` resource for every callback and engine call before writing one. Creating a file does not attach it: give an entity the `script` component with `scene_update` (`{"entity": "Spinner", "components": {"script": {"path": "spinner.lua", "properties": {"speed": 180}}}}`). Scripts run in play mode (`sim_play`).
