List the bodies a shape overlaps right now, deepest first: `{hits: [{entity, name, kind, layer, point, normal, penetration}], count}`. `penetration` is how deep (metres) the shapes overlap.

The shape is one of:

- `"sphere"` with `radius`;
- `"box"` with `half_extents` `[x, y, z]`;
- `"capsule"` with `radius` and `half_height` (half of its straight part, along local Y).

It sits at `position`, rotated by `rotation` (Euler degrees, as `transform.rotation`). A dimension the shape does not use is an error.

Use it before placing something ("is this spot free?"), to see what is inside an area, or to find what a trigger would catch. `layers` and `max_hits` work as in `physics_raycast_all`. Needs a running simulation.
