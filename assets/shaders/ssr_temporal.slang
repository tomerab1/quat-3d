// ssr_temporal.slang — temporal accumulation of the screen-space
// reflections (ROADMAP R21, SsrPass).
//
// The resolved reflection (rgb radiance, a confidence) blends with last
// frame's accumulation, reprojected by the surface's velocity (R11
// convention: uv_previous = uv - velocity) and clamped to the 3x3
// neighbourhood's range of this frame's resolve, so stale reflections and
// disocclusions cannot ghost; off-screen history and camera cuts restart
// from the current frame. `params2.z` is the history weight (0 = none).

import ssr_common;

ConstantBuffer<SsrConstants>            g_consts;
Texture2D                               g_current;
Texture2D                               g_history;
Texture2D                               g_velocity;
SamplerState                            g_linear;
[format("rgba16f")] RWTexture2D<float4> g_out;

[shader("compute")]
[numthreads(8, 8, 1)]
void compute_main(uint3 tid : SV_DispatchThreadID) {
    int2 size = int2(g_consts.screen.xy);
    if (tid.x >= uint(size.x) || tid.y >= uint(size.y)) {
        return;
    }
    int2 coord = int2(tid.xy);
    float4 current = g_current.Load(int3(coord, 0));
    float feedback = g_consts.params2.z;
    if (feedback <= 0.0) {
        g_out[coord] = current;
        return;
    }
    float4 lo = current;
    float4 hi = current;
    for (int y = -1; y <= 1; ++y) {
        for (int x = -1; x <= 1; ++x) {
            float4 c = g_current.Load(int3(clamp(coord + int2(x, y), int2(0, 0), size - 1), 0));
            lo = min(lo, c);
            hi = max(hi, c);
        }
    }
    float2 uv = (float2(coord) + 0.5) * g_consts.screen.zw;
    float2 prev_uv = uv - g_velocity.Load(int3(coord, 0)).xy;
    if (any(prev_uv < 0.0) || any(prev_uv > 1.0)) {
        g_out[coord] = current;
        return;
    }
    float4 history = clamp(g_history.SampleLevel(g_linear, prev_uv, 0.0), lo, hi);
    if (any(isnan(history)) || any(isinf(history))) {
        history = current;
    }
    g_out[coord] = lerp(current, history, feedback);
}
