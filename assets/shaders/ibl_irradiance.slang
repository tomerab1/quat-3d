// ibl_irradiance.slang — diffuse irradiance convolution (Phase 7.6).
//
// For each output cube texel (a surface normal N), integrate the incoming
// radiance over the cosine-weighted hemisphere around N. The result, sampled by
// N in the lighting pass, is the diffuse environment term. Convention matches the
// standard reference: bake E·π so the lighting pass uses `irradiance · albedo`.

import ibl_common;

struct IrrParams {
    uint face_size;
    uint pad0;
    uint pad1;
    uint pad2;
}
[[vk::push_constant]] IrrParams pc;

[[vk::binding(0)]] TextureCube              g_env;
[[vk::binding(1)]] SamplerState             g_samp;
[[vk::binding(2)]] RWTexture2DArray<float4> g_out;

[shader("compute")]
[numthreads(8, 8, 1)]
void compute_main(uint3 tid : SV_DispatchThreadID) {
    if (tid.x >= pc.face_size || tid.y >= pc.face_size) {
        return;
    }
    float2 uv = (float2(tid.xy) + 0.5) / float(pc.face_size);
    float3 N = cube_dir(tid.z, uv);

    float3 up = abs(N.y) < 0.999 ? float3(0.0, 1.0, 0.0) : float3(0.0, 0.0, 1.0);
    float3 right = normalize(cross(up, N));
    up = cross(N, right);

    float3 irradiance = float3(0.0, 0.0, 0.0);
    float sample_delta = 0.05;
    uint n = 0;
    for (float phi = 0.0; phi < 2.0 * IBL_PI; phi += sample_delta) {
        for (float theta = 0.0; theta < 0.5 * IBL_PI; theta += sample_delta) {
            // Spherical -> world, around N.
            float3 tangent = float3(sin(theta) * cos(phi), sin(theta) * sin(phi), cos(theta));
            float3 sample_dir = tangent.x * right + tangent.y * up + tangent.z * N;
            irradiance += g_env.SampleLevel(g_samp, sample_dir, 0.0).rgb * cos(theta) * sin(theta);
            n += 1;
        }
    }
    irradiance = IBL_PI * irradiance / float(max(n, 1u));
    g_out[tid] = float4(irradiance, 1.0);
}
