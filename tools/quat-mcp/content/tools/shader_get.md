Read one material shader: its metadata (`state`, `path`, `reloads`, and `diagnostics` / `log` from its last compile) as JSON, followed by the module's source text verbatim — edit that text and send the whole module back with `shader_update`.

`shader_get {"name": "standard"}` returns `lib/material.slang`, the reference for everything a material can use: `MaterialInput` (`uv`, `world_pos`, `normal`, `view_dir`, `time`, `params` incl. `shader_params`, `standard`), `SurfaceSample` (what `evaluate` returns), `PbrMaterialParams` and the `IMaterial` interface.
