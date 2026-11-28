// dof_prepare.slang — depth of field, step 1 of 5 (R20): half-resolution
// colour + CoC.
//
// One thread per half-resolution pixel reads its 2x2 display pixels and their
// circles of confusion (thin lens, from the closest depth of each pixel's
// render footprint; dof_common.slang), and stores those display-pixel CoCs
// for the composite. The half-
// resolution CoC is the 2x2's nearest-field value when any of it is in front
// of the focus plane (so foreground edges keep their blur), else the largest
// far value; colours are averaged with weights favouring taps whose CoC
// matches it (Jimenez 2014's bilateral downsample), which keeps sharp
// foreground colour out of the blurred background and vice versa.

import post_common;
import dof_common;

Texture2D g_color; // display extent, linear HDR
Texture2D g_depth; // render extent, [0, 1], jittered
[format("rgba16f")] RWTexture2D<float4> g_half; // rgb colour, a signed CoC (half px)
[format("r16f")] RWTexture2D<float> g_coc;      // display extent, signed CoC (display px)

[[vk::push_constant]] DofParams pc;

[shader("compute")]
[numthreads(8, 8, 1)]
void compute_main(uint3 tid : SV_DispatchThreadID) {
    const int2 half_px = int2(tid.xy);
    if (any(half_px >= int2(pc.half_size.xy))) return;
    const int2 display_max = int2(pc.sizes.xy) - 1;

    float3 color[4];
    float  coc[4];
    float  near_coc = 0.0;
    float  far_coc = 0.0;
    [unroll]
    for (int i = 0; i < 4; ++i) {
        const int2 q = half_px * 2 + int2(i & 1, i >> 1);
        const int2 p = min(q, display_max);
        color[i] = g_color.Load(int3(p, 0)).rgb;
        coc[i] = coc_at_display_pixel(pc, g_depth, p);
        if (all(q == p)) g_coc[p] = coc[i];
        near_coc = min(near_coc, coc[i]);
        far_coc = max(far_coc, coc[i]);
    }
    const float chosen = near_coc < -0.5 ? near_coc : far_coc;

    float3 sum = 0.0;
    float  weight = 0.0;
    [unroll]
    for (int i = 0; i < 4; ++i) {
        const float w = 1.0 / (1.0 + abs(coc[i] - chosen));
        sum += color[i] * w;
        weight += w;
    }
    g_half[half_px] = float4(sum / weight, chosen * 0.5);
}
