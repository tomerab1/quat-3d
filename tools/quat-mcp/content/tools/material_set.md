Tune the material of one entity (id or unique name). Give the factors to change in `material` and the ones to put back in `revert`; everything else keeps its value. Returns the result as `material_get`.

On a primitive, `base_color`, `metallic`, `roughness` and `emissive` edit the primitive itself (the same fields `scene_update` sets); every other factor, and every factor of a glTF/mesh material, becomes an override on this entity only — the shared material asset and other entities using it are not changed. The engine range-checks each value and rejects the whole call if one is wrong.

Examples:
- glossy red plastic: `{"entity": "Car_Body", "material": {"base_color": "#b01010", "metallic": 0, "roughness": 0.25, "clearcoat": 1, "clearcoat_roughness": 0.05}}`
- brushed metal: `{"entity": 12, "material": {"metallic": 1, "roughness": 0.45}}`
- green glass: `{"entity": "Bottle", "material": {"transmission": 1, "ior": 1.5, "roughness": 0.05, "attenuation_color": [0.4, 0.9, 0.5], "attenuation_distance": 0.2, "thickness": 0.05}}`
- glowing sign: `{"entity": "Sign", "material": {"emissive": [8, 3, 1]}}`
- undo: `{"entity": "Car_Body", "revert": ["clearcoat", "clearcoat_roughness"]}`
