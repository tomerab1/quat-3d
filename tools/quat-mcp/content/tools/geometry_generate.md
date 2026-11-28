Build a mesh at runtime from a generator spec and, with `id`, store it in the engine's dynamic mesh store. To show it, give an entity a `dynamic_mesh` component naming that id (`scene_create` / `scene_update`). Put the spec in the component's `generator` field instead, and the scene file rebuilds the mesh when it loads.

Generators (`spec.type`):
- `roof`: a straight-skeleton roof over a footprint `polygon` (counter-clockwise `[x, z]` plan points; clockwise `holes` for courtyards). `weights` has one entry per edge: 1 slopes at 45 degrees, 0 makes a vertical gable end, and values between give steeper or flatter pitches. Set `base_height` to the eaves height and `height_scale` to flatten (< 1) or steepen (> 1) the whole roof. `max_height` cuts a flat top.
- `sweep`: a cross-section `profile` (x sideways, y up) swept along a `path` of 3D points. Use it for walls, kerbs and rails. Set `closed_profile` for a solid wall and `closed_path` for a ring wall. Corners are mitred, so the wall keeps its thickness. Sample curves densely.
- `extrude`: a footprint raised by `height`, with an optional `taper` (an inset of the top). Use it for building walls, plinths and towers.
- `stone`: a seeded rock (`size` `[x, y, z]` in metres, `roundness`, `noise`, `chips`).

Without `id` the result is only measured (vertex and triangle counts, bounds), which is useful for checking a spec first. Errors name the offending field: an unclosed or clockwise footprint, a self-intersecting polygon (fix it with `geometry_polygon` `repair`), or a sharp corner over the miter limit.

The output is identical on every platform for the same spec. Example: a gabled 8 x 5 m house roof starting at 3 m: `{"spec": {"type": "roof", "polygon": [[0,0],[8,0],[8,5],[0,5]], "weights": [1,0,1,0], "base_height": 3, "height_scale": 0.8}, "id": "house_roof"}`.
