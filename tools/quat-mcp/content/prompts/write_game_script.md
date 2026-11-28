Write gameplay for the running quat-3d game as a Lua script, using the quat-mcp tools: {{ goal }}
{%- if entity %}

Attach it to this entity: {{ entity }}
{%- endif %}
{%- if path %}

Save it as `assets/scripts/{{ path }}`.
{%- endif %}

Work in small, verified steps:

1. Learn the API. Read the resource `quat://docs/scripting`: the callbacks (`on_start`, `on_update`, `on_trigger_enter`, ...), the entity, scene, physics, input, time and log calls, math types, limits and the sandbox. Use only what it lists; when unsure, check it again instead of guessing.
2. Look at the scene. `scene_list` and `scene_get` for the entities involved (names, components, positions); `script_list` and `script_get` for scripts you can reuse or follow (e.g. `coin_collector/`).
3. Write the script. Return a table of callbacks, put tunable values in `properties`, cache `scene.find` results in `on_start`, keep per-entity state on `self`. Use `script_check` on drafts, then `script_create` (new file) or `script_update` (after `script_get`). A syntax error names the line: fix it and call again.
4. Set up the entities. Attach with `scene_update`: `{"entity": ..., "components": {"script": {"path": "...", "properties": {...}}}}` (or `scene_create` / `scene_batch` for new entities). Physics needs a `collider` (+ `rigidbody` for moving bodies); triggers are colliders with `sensor: true`; walking characters need a `character` component.
5. Run it. `sim_play`, then `debug_log` with `category: "script"` (keep the `cursor` and pass it as `since_seq` next time): `print` / `log.*` output and errors with `file:line` land there. `script_list` shows which scripts stopped. `debug_watch` the entities to see positions and components change; `debug_pause` + `debug_step` go frame by frame; `screenshot` shows the result.
6. Iterate. `script_update` hot-reloads while playing (instances restart). Fix one problem at a time and re-check the log after each change.
7. Finish. `sim_stop` restores the scene as it was before Play (what scripts spawned or moved disappears, the script components stay). Use `scene_save` to keep the setup. Summarise what the script does, its properties, and how to play it (controls).
