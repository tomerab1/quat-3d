2D polygon operations on plan polygons (`[x, z]` metres), for laying out footprints, courtyards, paths and plots before generating geometry. `op` is one of:
- `offset`: grow (`delta` > 0) or shrink (`delta` < 0), with `miter`, `round` or `square` corners;
- `union`, `intersect`, `subtract`, `xor`: combine `subject` with `clip`;
- `repair`: turn rings of any orientation, with self-intersections or overlaps, into clean polygons (`fill`: `non_zero`, or `even_odd` so nested rings make holes);
- `simplify`: drop near-collinear points within `epsilon`.

Polygons are `{outer, holes}` objects. The result is `{polygons, area}`: outer rings counter-clockwise and holes clockwise, as `geometry_generate` roofs and extrusions expect. Coordinates snap to a 15 µm grid. The tool is read-only and changes nothing in the scene.

Example: a wall footprint 0.4 m inside a plot is `{"op": "offset", "subject": [{"outer": [[0,0],[10,0],[10,8],[0,8]]}], "delta": -0.4}`.
