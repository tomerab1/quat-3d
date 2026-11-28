// fog_integrate.slang — front-to-back integration of the froxel fog
// (ROADMAP R13.3, FogPass; Wronski 2014, Hillaire 2015). One thread per
// screen tile walks its FOG_Z slices, integrating each slice's scattering
// with Hillaire's energy-conserving form (S - S exp(-sigma_t d)) / sigma_t
// and writing, per slice, the in-scattered radiance and transmittance from
// the camera to the slice's far boundary (lib/fog.slang sample_fog).

import fog;

struct IntegratePush {
    float4 params; // x far distance (m), y pre-exposure, zw unused
}
[[vk::push_constant]] IntegratePush pc;

Texture3D                               g_scatter; // rgb sigma_s L, a sigma_t
[format("rgba16f")] RWTexture3D<float4> g_integrated;

[shader("compute")]
[numthreads(8, 8, 1)]
void compute_main(uint3 tid : SV_DispatchThreadID) {
    if (tid.x >= FOG_X || tid.y >= FOG_Y) return;
    float far_m = pc.params.x;
    float3 lum = float3(0.0, 0.0, 0.0);
    float trans = 1.0;
    float start = 0.0;
    for (uint z = 0; z < FOG_Z; ++z) {
        float end = fog_depth(float(z + 1) / float(FOG_Z), far_m);
        float4 s = g_scatter.Load(int4(int3(int2(tid.xy), int(z)), 0));
        float sigma_t = s.a;
        float dt = end - start;
        float step_trans = exp(-sigma_t * dt);
        float3 c = sigma_t > 1e-7 ? (s.rgb - s.rgb * step_trans) / sigma_t : s.rgb * dt;
        lum += trans * c;
        trans *= step_trans;
        g_integrated[uint3(tid.xy, z)] = float4(lum * pc.params.y, trans);
        start = end;
    }
}
