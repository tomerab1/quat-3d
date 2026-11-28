Change an existing terrain edit stamp by its `id` (from `terrain_edit_add` or `terrain_edit_list`). Give only the fields to change; the others keep their values, and the stamp keeps its id and its place in the order. Coordinates are world space, metres (`points` are `[x, z]`, `height` is world Y). Returns `{id, revision, count}`; the terrain re-applies the edits on the next frame.

Examples:
- raise a plinth by half a metre: `{"id": 3, "height": 23.0}`
- widen a path and soften its edge: `{"id": 5, "radius": 1.8, "falloff": 1.5}`
- move a circle: `{"id": 7, "points": [[42, -12]]}`

An unknown id is an error; `terrain_edit_list` shows the stamps.
