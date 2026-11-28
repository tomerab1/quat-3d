List the material shaders: the built-in `standard` material first, then every authored one in `assets/shaders/materials/`. Each entry has `name`, `builtin`, `path`, `state` and `errors`:

- `state`: `builtin`; `cooked` (built with the engine, not recompiled this run); `compiling`; `ok` (last compile succeeded and is live); `error` (the last compile failed — the previous version still runs; `shader_get` shows the diagnostics); `not_compiled` (no program and no compiler in this run).
- `can_compile` is false when the engine runs without shader hot reload; then `shader_create` / `shader_update` are unavailable.

Use it to pick a name that is free, or to find a material to reuse. Put a material on an entity with `scene_update` → `{"primitive": {"shader": "<name>"}}`.
