// dof_gather.slang — depth of field, step 4 of 5 (R20): the bokeh gather.
//
// Scatter-as-gather at half resolution (Jimenez 2014, "Next Generation Post
// Processing in Call of Duty: Advanced Warfare"): each pixel gathers a disc
// of concentric rings (8 i samples on ring i, so every sample stands for the
// same area) whose radius is the dilated tile's largest CoC. A sample
// contributes where its own CoC covers the distance to the pixel — that is,
// where it would have scattered onto it:
//
//   * far field (behind the focus plane): a normalised average; a sample may
//     not spread further than the pixel's own blur allows (a closer surface
//     occludes what is behind it), which also keeps in-focus pixels free of
//     background bleeding;
//   * near field (in front): a separate layer with its own colour and an
//     opacity = covered area / the samples' scatter area, so a blurred
//     foreground fades out over its CoC and bleeds over the in-focus
//     background behind it.
//
// Outputs: g_far (rgb) and g_near (rgb, a opacity), composited over the
// full-resolution image by dof_composite.slang.

import post_common;
import dof_common;

Texture2D                g_half;    // rgb colour, a signed CoC (half px)
Texture2D<float2>        g_dilated; // r near radius, g far radius (half px)
[format("rgba16f")] RWTexture2D<float4> g_near;
[format("rgba16f")] RWTexture2D<float4> g_far;

[[vk::push_constant]] DofParams pc;

static const float PI = 3.14159265;

[shader("compute")]
[numthreads(8, 8, 1)]
void compute_main(uint3 tid : SV_DispatchThreadID) {
    const int2 p = int2(tid.xy);
    const int2 half_size = int2(pc.half_size.xy);
    if (any(p >= half_size)) return;

    const float4 center = g_half.Load(int3(p, 0));
    const float2 tile = g_dilated.Load(int3(p / DOF_TILE, 0));
    const float radius = max(max(tile.x, tile.y), abs(center.a));
    if (radius < 0.5) {
        g_far[p] = float4(center.rgb, 1.0);
        g_near[p] = float4(center.rgb, 0.0);
        return;
    }

    const int rings = clamp(int(pc.coc.w), 1, 8);
    const float far_limit = max(center.a, 0.0) + 1.0; // occlusion: see the header
    float3 far_sum = 0.0;
    float  far_w = 0.0;
    float3 near_sum = 0.0;
    float  near_w = 0.0;
    float  near_alpha = 0.0;
    float  count = 0.0;

    // Ring 0 is the pixel itself.
    for (int ring = 0; ring <= rings; ++ring) {
        const int   n = ring == 0 ? 1 : 8 * ring;
        const float dist = radius * float(ring) / float(rings);
        const float phase = float(ring) * 0.5; // stagger the rings' spokes
        for (int k = 0; k < n; ++k) {
            const float a = phase + 2.0 * PI * float(k) / float(n);
            const int2 q = clamp(p + int2(round(float2(cos(a), sin(a)) * dist)), int2(0, 0),
                                 half_size - 1);
            const float4 s = g_half.Load(int3(q, 0));
            const float wf = saturate(min(max(s.a, 0.0), far_limit) - dist + 1.0);
            far_sum += s.rgb * wf;
            far_w += wf;
            const float near_r = max(-s.a, 0.0);
            const float wn = saturate(near_r - dist + 1.0);
            near_sum += s.rgb * wn;
            near_w += wn;
            // Share of the sample's scatter disc this pixel's sample covers.
            const float spread = radius / max(near_r, 1.0);
            near_alpha += wn * spread * spread;
            count += 1.0;
        }
    }
    g_far[p] = float4(far_w > 0.0 ? far_sum / far_w : center.rgb, 1.0);
    g_near[p] = float4(near_w > 0.0 ? near_sum / near_w : center.rgb,
                       saturate(near_alpha / count));
}
