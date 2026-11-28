// dof_composite.slang — depth of field, step 5 of 5 (R20): composite.
//
// Per display pixel (its CoC as dof_prepare stored it): the full-resolution
// colour where the pixel is in focus,
// the half-resolution far field as its own CoC grows (sharp below half a
// pixel, fully blurred from two pixels up), then the near field over both
// by its opacity (which also covers in-focus pixels a blurred foreground
// bleeds onto). The half-resolution layers are upsampled bilinearly.

import post_common;
import dof_common;

Texture2D    g_color; // display extent, linear HDR (sharp)
Texture2D    g_coc;   // display extent, signed CoC (display px; dof_prepare)
Texture2D    g_near;  // half extent, rgb near colour, a opacity
Texture2D    g_far;   // half extent, rgb far colour
SamplerState g_linear;
[format("rgba16f")] RWTexture2D<float4> g_out;

[[vk::push_constant]] DofParams pc;

[shader("compute")]
[numthreads(8, 8, 1)]
void compute_main(uint3 tid : SV_DispatchThreadID) {
    const int2 p = int2(tid.xy);
    if (any(p >= int2(pc.sizes.xy))) return;
    const float2 uv = (float2(p) + 0.5) / pc.sizes.xy;
    const float3 sharp = g_color.Load(int3(p, 0)).rgb;
    const float coc = g_coc.Load(int3(p, 0)).r;

    const float3 far = g_far.SampleLevel(g_linear, uv, 0.0).rgb;
    float3 c = lerp(sharp, far, smoothstep(0.5, 2.0, coc));
    const float4 near = g_near.SampleLevel(g_linear, uv, 0.0);
    c = lerp(c, near.rgb, near.a);
    g_out[p] = float4(c, 1.0);
}
