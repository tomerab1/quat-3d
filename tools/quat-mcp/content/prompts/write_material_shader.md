Write a material shader for the running quat-3d engine with the quat-mcp tools.

The look: {{ goal }}
{%- if entity %}

Put it on this entity: {{ entity }}
{%- endif %}

A material shader is a Slang module that computes a surface — albedo, normal, metallic, roughness, occlusion, emission, alpha — for every pixel of the entities using it. The engine compiles it, swaps it into the running renderer, and lights, shadows and blooms it like every other surface.

1. Start from the template. `shader_template` returns a compiling module whose comments list every input and output. `shader_get {"name": "standard"}` shows the full type definitions (`MaterialInput`, `SurfaceSample`, `PbrMaterialParams`) if you need them. `shader_list` shows which names are taken.
2. Write the module. Keep `import material;`, one `struct ... : IMaterial` with `static SurfaceSample evaluate(MaterialInput input)`, and the last line `export struct SurfaceMaterial : IMaterial = <YourStruct>;`. Start from `SurfaceSample s = input.standard;` and change what the look needs. Use `input.uv` or `input.world_pos` for patterns (write your own hash / value-noise helpers — there are no textures), `input.time` (seconds) for animation, `input.view_dir` and `input.normal` for rim or fresnel effects, `emissive` (linear HDR, values above 1 glow) for light-emitting parts, and `input.params.shader_params[0]` / `[1]` for knobs you want to tune per entity. Keep colours in 0..1 linear; alpha below 0.5 cuts the pixel out.
3. Compile. `shader_create {"name": ..., "source": ...}` with a lowercase name. If it fails, read each `file:line:column` message with its caret, fix exactly those lines and call `shader_create` again (nothing was saved). Do not guess at unrelated changes.
4. Assign it.{% if entity %} `scene_update` the entity above{% else %} Pick or create an entity (`scene_list`; or `scene_create` with a `primitive` sphere or cube in front of the camera){% endif %} with `{"components": {"primitive": {"shader": "<name>", "shader_params": [...]}}}`. The primitive's `color`, `metallic`, `roughness` and `emissive` arrive in `input.params` too.
5. Look at it. `camera_frame` the entity, then `screenshot`. Compare with the goal: colour, scale of the pattern, brightness of the glow, motion.
6. Iterate. Change the source and send the whole module with `shader_update` (on failure the previous version keeps running, so the scene never breaks), or tune `shader_params` with `scene_update` for quick adjustments. Screenshot after each change. Stop when it matches the goal, and report the material name, what the parameters do, and the entities using it.
