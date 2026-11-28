// Test material: flat_glow with albedo and emission swapped — the edit the
// GPU test hot-swaps in.

import material;

struct FlatGlowSwapped : IMaterial {
    static SurfaceSample evaluate(MaterialInput input) {
        SurfaceSample s = input.standard;
        s.albedo = input.params.shader_params[1].rgb;
        s.emissive = input.params.shader_params[0].rgb;
        s.metallic = 0.0;
        s.roughness = 1.0;
        s.normal_ts = float3(0.0, 0.0, 1.0);
        s.alpha = 1.0;
        return s;
    }
}

export struct SurfaceMaterial : IMaterial = FlatGlowSwapped;
