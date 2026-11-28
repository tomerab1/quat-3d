---
name: author-material-shader
description: Author surface materials in a running quat-3d scene - painted metal, plastic, wood, stone, car paint with clearcoat, glass and liquids with transmission, glowing emissive signs - by tuning the PBR material of each entity against reference values, and write custom Slang material shaders (procedural patterns, animated lava, force fields, rim glow) that the engine compiles and hot-swaps, checked with screenshots under controlled lighting. Use when asked to make something look like a specific material, to create a custom or animated look, or to fix a surface that reads wrong in quat-3d.
compatibility: Needs the quat-mcp MCP server connected to a running quat-3d engine (started with QUAT_REMOTE).
metadata:
  quat-mcp-tools: "scene_list scene_get scene_create scene_update material_get material_set render_settings_schema render_settings_get environment_get environment_set camera_set camera_frame screenshot shader_template shader_list shader_get shader_create shader_update shader_delete scene_save"
---

# Author a material

quat-3d shades every surface with a metallic-roughness PBR material. Over
MCP there are two ways to author it:

- **Material factors** (`material_set`) for real-world materials: metal,
  plastic, wood, car paint, glass, liquids. They work on primitives and glTF
  meshes, per entity, and leave the shared material asset alone. Start
  here: most looks are a set of factors.
- **Material shaders** (`shader_*`) for looks factors cannot express:
  procedural patterns (stripes, tiles, noise, cracks), animation (flowing
  lava, pulsing glow, scrolling energy), view-dependent effects (rim light,
  fresnel glow). A material shader is a Slang module the engine compiles,
  hot-swaps into the running renderer and lights, shadows and blooms like
  every other surface. It applies to `primitive` and `instanced_mesh`
  primitives, not to glTF meshes.

## Workflow: material factors

1. **Find the surface.** `scene_list` / `scene_get` for the entity. Then
   `material_get`: `source` is `primitive` or `mesh` (glTF); `overridden`
   lists factors already tuned on this entity; `textures` names the texture
   slots the material samples (a textured factor multiplies its texture);
   `shared_by` is how many entities draw the same asset.
2. **Learn the ranges.** `render_settings_schema` once: its `material`
   table lists every factor `material_set` accepts with kind, range and
   hint.
3. **Control the lighting.** Judge materials in neutral daylight before the
   final mood: `environment_get`, then if needed `environment_set`
   `{"sun": {"elevation_deg": 45}}`. Check `render_settings_get`
   `exposure` - a wrongly exposed image makes every material read wrong
   (fix exposure first; the tune-look skill covers it). `camera_set` close
   enough that the surface fills a good part of the frame, at an angle that
   shows a highlight.
4. **Set the base material** with one `material_set`, starting from the
   reference values below, then `screenshot`.
5. **Refine one factor at a time** (roughness first: it decides how the
   surface reads more than colour does), screenshot after each change, and
   put back anything that made it worse with `revert`.
6. **Check it in context.** Restore the user's lighting
   (`environment_set` with the values you read), screenshot again from the
   normal view. Summarise the final factors; `scene_save` keeps material
   overrides with the scene, only if asked.

## Workflow: material shader

1. **Start from the template.** `shader_template` returns a compiling
   module whose comments list every input and output. `shader_get`
   `{"name": "standard"}` shows the full type definitions (`MaterialInput`,
   `SurfaceSample`, `PbrMaterialParams`); `shader_list` shows which names
   are taken and whether the engine can compile (`can_compile`).
2. **Write the module.** Keep `import material;`, one
   `struct ... : IMaterial` with
   `static SurfaceSample evaluate(MaterialInput input)`, and the last line
   `export struct SurfaceMaterial : IMaterial = <YourStruct>;`. Start from
   `SurfaceSample s = input.standard;` and change only what the look needs:
   - `input.uv` or `input.world_pos` for patterns (write your own hash and
     value-noise helpers: there are no textures);
   - `input.time` (seconds) for animation;
   - `input.view_dir` and `input.normal` for rim or fresnel effects;
   - `emissive` (linear HDR; above 1 glows through bloom) for parts that
     emit light;
   - `input.params.shader_params[0]` / `[1]` for knobs to tune per entity.

   Keep colours in 0..1 linear and roughness in the ranges of the reference
   table below, so the result still reads as a physical surface.
3. **Compile.** `shader_create` `{"name": ..., "source": ...}` with a
   lowercase name. On failure, read each `file:line:column` message and its
   caret, fix exactly those lines and call `shader_create` again (nothing was
   saved). Do not guess at unrelated changes.
4. **Assign it.** Pick or create the entity (`scene_list`; or
   `scene_create` a `primitive` sphere or cube in front of the camera), then
   `scene_update` it with
   `{"components": {"primitive": {"shader": "<name>", "shader_params": [...]}}}`.
   The primitive's `color`, `metallic`, `roughness` and `emissive` arrive in
   `input.params` too.
5. **Look at it.** Control the lighting as in step 3 of the factors
   workflow, `camera_frame` the entity, then `screenshot`. Compare with the
   goal: colour, scale of the pattern, brightness of the glow, motion
   (take two screenshots a second apart for animation).
6. **Iterate.** Change the source and send the whole module with
   `shader_update` (on failure the previous version keeps running, so the
   scene never breaks), or tune `shader_params` with `scene_update` for
   quick adjustments. Screenshot after each change. Report the material
   name, what each parameter does and the entities using it.

## Reference values

Base colours are sRGB hex; real albedos sit between about `#1e1e1e`
(charcoal) and `#f0f0f0` (fresh snow) - avoid pure black and pure white.
Metallic is 0 for non-metals and 1 for bare metal; values in between are only
for blends such as dusty or partly painted metal.

| Look | Factors |
|---|---|
| Matte plastic | `base_color` any, `metallic` 0, `roughness` 0.5-0.7 |
| Glossy plastic | `metallic` 0, `roughness` 0.15-0.3 |
| Car paint | `metallic` 0, `roughness` 0.3, `clearcoat` 1, `clearcoat_roughness` 0.03-0.08 |
| Polished metal | `metallic` 1, `roughness` 0.05-0.2; gold `#ffe39d`, copper `#fad1c2`, silver `#fcfaf5`, aluminium `#f5f6f6`, iron `#c6c6c8` |
| Brushed / worn metal | `metallic` 1, `roughness` 0.35-0.6 |
| Wood, stone, concrete | `metallic` 0, `roughness` 0.6-0.9 |
| Rubber, fabric | `metallic` 0, `roughness` 0.85-1 |
| Clear glass | `transmission` 1, `ior` 1.5, `roughness` 0-0.05, `thickness` a few cm |
| Tinted glass, liquids | as glass plus `attenuation_color` (the tint) and `attenuation_distance` (how deep until it is reached); water `ior` 1.33 |
| Emissive sign or lamp | `emissive` linear [r, g, b] above 1 (e.g. [8, 3, 1]); base colour dark |

## Verify

- `material_get` lists exactly the factors you meant to change in
  `overridden`, with the values you set.
- For a shader: `shader_list` shows its `state` as `ok` with no `errors`,
  and `scene_get` shows `primitive.shader` set on every entity meant to use
  it.
- A screenshot in neutral light and one in the scene's own light both read
  as the intended material: highlight size matches the roughness, metals
  reflect the sky, glass shows what is behind it.

## Pitfalls

- On a primitive, `base_color`, `metallic`, `roughness` and `emissive`
  edit the primitive itself (as `scene_update` would); every other factor,
  and every factor of a glTF material, becomes an override on this entity
  only. Other entities sharing the asset keep their look.
- Instanced meshes (`instanced_mesh`) have no per-entity material: change
  their primitive fields (`color`, `metallic`, `roughness`, `emissive`) with
  `scene_update`.
- Emission above 1 only glows through bloom; if a glowing surface looks
  flat, check that bloom is enabled.
- Transparency of a primitive comes from the alpha of its colour
  (`"#rrggbbaa"`); `alpha_cutoff` only affects textured, alpha-masked
  materials. Glass should use `transmission`, not alpha.
- The engine range-checks each factor and rejects the whole call if one is
  wrong; the error names it.
- A material shader cannot use `#include` or other preprocessor
  directives, import files by path, or declare textures, buffers or global
  parameters: everything comes in through `MaterialInput`. A compile error
  without a location usually means the `export struct SurfaceMaterial`
  line is missing or the module declares its own parameters.
- With a shader the object stays opaque: glass and liquids need
  `transmission` factors on the standard material, not a shader.
- `shader_delete` resets every entity using the shader to `standard` and
  cannot be undone; read the source with `shader_get` first if it may be
  needed again.
