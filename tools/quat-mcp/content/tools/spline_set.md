Edit an entity's spline, or give it one. Fields you leave out keep their values; `points` replaces every point.

- `kind`: `centripetal_catmull_rom` (default) is a smooth curve through every point, with no loops or cusps. `cubic_bezier` takes anchor, handle, handle, anchor, … (3k + 1 points, or 3k when closed). `polyline` draws straight segments.
- `points`: entity-local metres, each `[x, y, z]` or `{"position": [x, y, z], "width": 2, "height": 1.5, "tags": 0}`. Width is across the curve (a path's or wall's thickness) and height is above it (a wall's height).
- `closed` loops back to the first point. `follow_ground` (default true) drapes samples on the terrain, plus `ground_offset` metres.
- `network` puts splines with the same non-zero number into one curve network (junctions, crossings).

The whole spline is validated, so an impossible edit is refused with the field at fault and changes nothing. Example: `{"entity": "wall", "kind": "polyline", "points": [[0, 0, 0], [8, 0, 0], {"position": [8, 0, 6], "height": 3}]}`.
