Cast a ray and report everything it passes through, nearest first: `{hits: [{entity, name, kind, layer, point, normal, fraction, distance}], count}`. The reply has one hit per body. `kind` is `body`, `character` or `static_world` (terrain or geometry no entity owns, `entity` null). `fraction` is the share of `max_distance` (default 1000 m) at the hit.

`layers` picks what can be hit, from `static`, `dynamic`, `character`, `trigger` and `sensor`. The default is `["static", "dynamic", "character"]`; add `"sensor"` to see trigger volumes. `max_hits` defaults to 32 (at most 256). Needs a running simulation (`sim_play`).

Use it to find everything stacked under a point, what stands between two objects, or which layers a line of sight crosses. For just the nearest hit, `debug_raycast` is lighter.
