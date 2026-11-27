Run many commands in one engine frame — the efficient way to build. Each command is `{method, params}` where method is an engine method (scene.create, scene.update, `scene.remove_components`, scene.destroy, `scene.set_parent`, scene.clear, environment.set, camera.set, camera.frame, `world.ground_height`, …) and params is exactly what the matching tool takes.

In entity/parent fields, the string `"@N"` means the id returned by command N (0-based) of this batch, so you can create a parent and its children together:

```json
[{"method": "scene.create", "params": {"name": "Table",
   "components": {"transform": {"position": [0, 0, 0]}}}},
 {"method": "scene.create", "params": {"name": "Table_Top", "parent": "@0",
   "components": {"transform": {"position": [0, 0.75, 0], "scale": [1.6, 0.06, 0.9]},
                  "primitive": {"shape": "cube", "color": "#8b5a2b"}}}}]
```

Returns `{results: [{ok: true, result} | {ok: false, error: {code, message}}]}` in command order. With `stop_on_error` (default true) the batch stops at the first failure and later commands do not run (earlier ones stay applied). Screenshots cannot be batched. Keep batches to a few hundred commands.
