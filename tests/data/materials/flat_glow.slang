// Test material: a flat surface whose albedo is shader_params[0].rgb and
// whose emission is shader_params[1].rgb — exact values the GPU test reads
// back from the GBuffer.

import material;

struct FlatGlow : IMaterial {
    static SurfaceSample evaluate(MaterialInput input) {
        SurfaceSample s = input.standard;
        s.albedo = input.params.shader_params[0].rgb;
        s.emissive = input.params.shader_params[1].rgb;
        s.metallic = 0.0;
        s.roughness = 1.0;
        s.normal_ts = float3(0.0, 0.0, 1.0);
        s.alpha = 1.0;
        return s;
    }
}

export struct SurfaceMaterial : IMaterial = FlatGlow;
