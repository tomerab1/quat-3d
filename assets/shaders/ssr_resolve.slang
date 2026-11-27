// ssr_resolve.slang — screen-space reflection radiance with ray reuse
// (ROADMAP R21, SsrPass; Stachowiak 2015).
//
// Each pixel reuses the hits of itself and up to four neighbours (a pattern
// that alternates per frame): the direction from this pixel to a
// neighbour's hit is weighted by this pixel's GGX lobe over the pdf the
// neighbour drew it with (D Vis NdotL / pdf), which turns one ray per pixel
// into a several-sample estimate of the pixel's own lobe — Fresnel stays in
// the lighting pass's split sum, so the output is lobe-averaged radiance.
// The radiance comes from the previous frame's lit HDR at the hit point's
// reprojected position (velocity at the hit). Output: rgb radiance, a
// confidence (the fraction of the lobe the hits cover, faded toward the
// roughness cutoff); the lighting pass blends the environment under it.

import ssr_common;
import octahedral;
import hdr;

ConstantBuffer<SsrConstants>            g_consts;
Texture2D                               g_hits;
Texture2D                               g_depth;
Texture2D                               g_normal;
Texture2D                               g_material;
Texture2D                               g_velocity;
Texture2D                               g_color; // last frame's lit HDR (render extent)
SamplerState                            g_linear;
[format("rgba16f")] RWTexture2D<float4> g_out;

static const int2 SSR_REUSE[2][4] = {
    { int2(2, 1), int2(-1, 2), int2(-2, -1), int2(1, -2) },
    { int2(1, 2), int2(-2, 1), int2(-1, -2), int2(2, -1) },
};

[shader("compute")]
[numthreads(8, 8, 1)]
void compute_main(uint3 tid : SV_DispatchThreadID) {
    int2 size = int2(g_consts.screen.xy);
    if (tid.x >= uint(size.x) || tid.y >= uint(size.y)) {
        return;
    }
    int2 coord = int2(tid.xy);
    float4 mat = g_material.Load(int3(coord, 0));
    float roughness = mat.g;
    if (mat.a < 0.5 || roughness > g_consts.params.x || g_consts.params2.y < 0.5) {
        g_out[coord] = float4(0.0, 0.0, 0.0, 0.0);
        return;
    }
    float2 uv = (float2(coord) + 0.5) * g_consts.screen.zw;
    float3 p = ssr_world(g_consts.inv_view_proj, uv, g_depth.Load(int3(coord, 0)).r);
    float3 v = normalize(g_consts.camera_pos.xyz - p);
    float3 n = oct_decode(g_normal.Load(int3(coord, 0)).xy);
    float a = max(roughness * roughness, 1e-3);
    float ndotv = max(dot(n, v), 1e-4);

    uint reuse = uint(g_consts.params2.w);
    uint pattern = uint(g_consts.camera_pos.w) & 1u;
    float3 radiance = float3(0.0, 0.0, 0.0);
    float weight_sum = 0.0;
    float confidence_sum = 0.0;
    for (uint k = 0; k <= reuse; ++k) {
        int2 q = k == 0u ? coord : clamp(coord + SSR_REUSE[pattern][k - 1u], int2(0, 0), size - 1);
        float4 h = g_hits.Load(int3(q, 0));
        if (h.w <= 0.0) {
            continue;
        }
        int2 hit_texel = clamp(int2(h.xy * g_consts.screen.xy), int2(0, 0), size - 1);
        float3 hit_p = ssr_world(g_consts.inv_view_proj, h.xy,
                                 g_depth.Load(int3(hit_texel, 0)).r);
        float3 l = normalize(hit_p - p);
        float ndotl = dot(n, l);
        if (ndotl <= 0.0) {
            continue;
        }
        float3 hv = normalize(v + l);
        float brdf = ssr_d_ggx(saturate(dot(n, hv)), a) * ssr_vis(ndotv, ndotl, a) * ndotl;
        float w = brdf / max(h.z, 1e-4);
        float2 prev_uv = h.xy - g_velocity.Load(int3(hit_texel, 0)).xy;
        if (any(prev_uv < 0.0) || any(prev_uv > 1.0) || !(w > 0.0) || isinf(w)) {
            continue;
        }
        float3 c = g_color.SampleLevel(g_linear, prev_uv, 0.0).rgb;
        if (any(isnan(c)) || any(isinf(c))) {
            continue;
        }
        radiance += c * (w * h.w);
        weight_sum += w * h.w;
        confidence_sum += h.w;
    }
    float confidence = confidence_sum / float(reuse + 1u);
    float3 result = weight_sum > 0.0 ? radiance / weight_sum : float3(0.0, 0.0, 0.0);
    confidence *= ssr_roughness_fade(roughness, g_consts.params.x);
    g_out[coord] = float4(hdr_store(result), weight_sum > 0.0 ? confidence : 0.0);
}
