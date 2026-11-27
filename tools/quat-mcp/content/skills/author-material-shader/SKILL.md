---
name: author-material-shader
description: Author surface materials in a running quat-3d scene - painted metal, plastic, wood, stone, car paint with clearcoat, glass and liquids with transmission, glowing emissive signs - by tuning the PBR material of each entity against reference values and screenshots under controlled lighting. Use when asked to make something look like a specific material or to fix a surface that reads wrong in quat-3d.
compatibility: Needs the quat-mcp MCP server connected to a running quat-3d engine (started with QUAT_REMOTE).
metadata:
  quat-mcp-tools: "scene_list scene_get scene_update material_get material_set render_settings_schema render_settings_get environment_get environment_set camera_set screenshot scene_save"
---

# Author a material

quat-3d shades every surface with a metallic-roughness PBR material. Over
MCP you author it per entity: `material_set` writes factors on one entity and
leaves the shared material asset alone. (Writing new Slang material shaders
is not exposed over MCP yet; this skill covers the material model.)

## Workflow

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
