// atmos_transmittance.slang — transmittance LUT (Phase 11, Slice 11.1).
//
// For every (altitude, sun-zenith) pair: optical depth from the point to the
// top of the atmosphere, integrated through the Rayleigh/Mie/ozone medium.
// Static — computed once at startup.

import atmosphere;

[[vk::binding(0)]] RWTexture2D<float4> g_out;

static const int STEPS = 40;

[shader("compute")]
[numthreads(8, 8, 1)]
void compute_main(uint3 tid : SV_DispatchThreadID) {
    uint width = 0;
    uint height = 0;
    g_out.GetDimensions(width, height);
    if (tid.x >= width || tid.y >= height) return;

    float2 uv = (float2(tid.xy) + 0.5) / float2(width, height);
    float r = 0.0;
    float mu = 0.0;
    transmittance_params_from_uv(uv, r, mu);

    float d = distance_to_top(r, mu);
    float dt = d / STEPS;

    float3 optical_depth = float3(0.0, 0.0, 0.0);
    for (int i = 0; i < STEPS; ++i) {
        float t = (float(i) + 0.5) * dt;
        float rt = sqrt(r * r + t * t + 2.0 * r * t * mu);
        MediumSample m = sample_medium(rt - ATMOS_GROUND_RADIUS);
        optical_depth += m.extinction * dt;
    }
    g_out[tid.xy] = float4(exp(-optical_depth), 1.0);
}
