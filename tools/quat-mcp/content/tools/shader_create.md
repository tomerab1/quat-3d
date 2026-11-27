Add a new material shader and compile it in the running engine. `name` becomes `assets/shaders/materials/<name>.slang` (lowercase letters, digits, `_`; `standard` and library module names such as `material` or `pbr` are taken); `source` is the whole Slang module — start from `shader_template`.

The call returns when the compile has finished:

- Success: the material is live. Assign it with `scene_update {"entity": ..., "components": {"primitive": {"shader": "<name>", "shader_params": [...]}}}`, then `screenshot`.
- Failure: an error result listing each compiler message as `file:line:column: error[code]: message`, followed by the offending source line and a caret under the column. Nothing was saved — fix the source and call `shader_create` again with the same name. A message without a location usually means the module does not end with `export struct SurfaceMaterial : IMaterial = <YourStruct>;`, or declares its own textures / parameters (not allowed: use `MaterialInput`).

If the name already exists, use `shader_update`.
