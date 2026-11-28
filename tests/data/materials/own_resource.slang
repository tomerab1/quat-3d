// Test material that declares its own texture: it compiles, but the program
// needs a binding the GBuffer pass's pipeline layout does not have.

import material;

Texture2D    g_extra;
SamplerState g_extra_sampler;

struct OwnResource : IMaterial {
    static SurfaceSample evaluate(MaterialInput input) {
        SurfaceSample s = input.standard;
        s.albedo = g_extra.Sample(g_extra_sampler, input.uv).rgb;
        return s;
    }
}

export struct SurfaceMaterial : IMaterial = OwnResource;
