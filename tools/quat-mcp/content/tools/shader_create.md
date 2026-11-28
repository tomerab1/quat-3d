Add a new material shader and compile it in the running engine. `name` becomes `assets/shaders/materials/<name>.slang` (lowercase letters, digits, `_`; `standard` and library module names such as `material` or `pbr` are taken); `source` is the whole Slang module — start from `shader_template`.

The call returns when the compile has finished:

- Success: the material is live. Assign it with `scene_update {"entity": ..., "components": {"primitive": {"shader": "<name>", "shader_params": [...]}}}`, then `screenshot`.
- Failure: an error result listing each compiler message as `file:line:column: error[code]: message`, followed by the offending source line and a caret under the column. Nothing was saved — fix the source and call `shader_create` again with the same name. A message without a location usually means the module does not end with `export struct SurfaceMaterial : IMaterial = <YourStruct>;`, or declares its own textures / parameters (not allowed: use `MaterialInput`).

Before compiling, the engine refuses sources that could read other files or take the GPU down. The error names the line and the construct:

- no `#` directives, no `import "<path>"` (import library modules by name, e.g. `import material;`), and no `__`-prefixed identifiers;
- no inline SPIR-V (`spirv_asm`), no `vk::` attributes or intrinsics, no pointers (`T*`, `Ptr<T>`, `*p`, `&x`, `->`) and no buffer device addresses (`ConstBufferPointer`, `RawBufferLoad`).

**Loops need compile-time bounds**, for example `for (int i = 0; i < 4; ++i)`. The shader runs for every pixel, so a loop whose end depends on data can stall the GPU. Endless forms such as `for (;;)` or `while (true)` are refused. Slang has no attribute that caps a loop at run time: `[MaxIters(N)]` and `[unroll]` are only optimisation hints. If a shader does hang the GPU, the device is lost and the engine exits. Before it exits, it writes a recovery save of the scene to `recovery/` in the project.

If the name already exists, use `shader_update`.
