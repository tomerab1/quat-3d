Add an edit stamp to the terrain: a shape on the ground plus an operation applied inside it. Stamps apply after the terrain is generated, in list order, on the next frame; from then on ground heights (`world_ground_height`, `world_place_on_ground`), scatters, grass, physics and the navmesh see the edited ground. Returns `{id, revision, count}`. The id is stable and never reused: keep it to change the stamp with `terrain_edit_update`, or to undo it with `terrain_edit_remove`. Stamps are saved with the scene (the Terrain entity's `terrain_edits` component).

All coordinates are **world space**, metres: `points` are `[x, z]`, `height` is world Y.

Shapes (`shape` + `points` + `radius`):
- `circle`: one point, the centre; `radius` is the circle's radius.
- `capsule`: a polyline of 1+ points swept by a disc; `radius` is the half-width (paths, roads, ditches).
- `polygon`: a closed ring of 3+ points (do not repeat the first point); `radius` grows it outward (plinths, yards, fields).
- `falloff`: metres beyond the shape over which the effect fades to 0 with a smoothstep. Use 1-4 m so the edit blends into the ground; 0 leaves a hard edge. `radius + falloff` is at most 4096.

Operations (`op`), each blended by `strength` 0..1 (default 1):
- `flatten`: pull the ground to `height` (world Y). Level by default; with a tilted upward `normal` `[x, y, z]` the target is the plane through (`pivot`, `height`), for ramps and sloped yards.
- `raise` / `lower`: add / remove `amount` metres (0..1000), e.g. a mound or a pond basin.
- `smooth`: blur the heights, `iterations` 1..16 passes of a 3x3 filter; softens edges left by other stamps.
- `paint`: move a surface channel toward `value` 0..1: `dirt`, `gravel`, `soil`, or `grass` (the grass-density mask: `value` 0 clears grass, 1 lets it grow back).

Order matters: a stamp sees the result of the ones before it. `before` (a stamp id) inserts the new stamp ahead of that one; omit it to append. Read the current ground first with `world_ground_height` to pick a sensible `height`.

Examples:
- flatten a house plinth at 22.5 m: `{"shape": "polygon", "points": [[10, 4], [18, 4], [18, 10], [10, 10]], "radius": 1, "falloff": 2, "op": "flatten", "height": 22.5}`
- paint a dirt path: `{"shape": "capsule", "points": [[0, 0], [12, 3], [25, 2]], "radius": 1.2, "falloff": 0.8, "op": "paint", "channel": "dirt", "value": 1}`, then clear the grass on it with the same shape and `"channel": "grass", "value": 0`, and optionally soften the ground under it with `"op": "smooth", "iterations": 2`
- a small hill: `{"shape": "circle", "points": [[40, -15]], "radius": 6, "falloff": 10, "op": "raise", "amount": 4}`

A bad field comes back naming it (`op: expected one of flatten, raise, lower, smooth, paint`); a scene without a terrain is `not_found`. Check the result with `screenshot`.
