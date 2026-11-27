// ssr_trace.slang — screen-space reflection rays (ROADMAP R21, SsrPass).
//
// Per pixel below the roughness cutoff: one reflected direction drawn from
// the GGX distribution of visible normals (Heitz 2018), projected to a
// screen-space ray (uv, NDC depth — depth is affine along a projected line)
// and marched through the Hi-Z pyramid's nearest-depth channel: a cell the
// ray passes above is skipped and the march climbs a level; a cell it would
// pass below descends a level, until it lands on the depth surface at mip 0
// (Uludag 2014; the traversal is AMD FidelityFX SSSR's min-Z march, standard
// depth). Hits are validated (on screen, not the starting pixel, not the
// background, not a back face, within `thickness` of the surface) and faded
// at the screen edges. Output: xy hit uv, z the direction's pdf (the
// resolve's ray reuse weights by it), w confidence (0 = miss).

import ssr_common;
import octahedral;

ConstantBuffer<SsrConstants>            g_consts;
Texture2D<float2>                       g_hiz; // r = min (nearest) depth, every mip
Texture2D                               g_depth;
Texture2D                               g_normal;
Texture2D                               g_material;
[format("rgba32f")] RWTexture2D<float4> g_hits;

static const float SSR_FAR = 1.0e30;

float load_hiz(float2 mip_pos, int mip) {
    int2 size = max(int2(g_consts.screen.xy) >> mip, int2(1, 1));
    int2 p = clamp(int2(floor(mip_pos)), int2(0, 0), size - 1);
    return g_hiz.Load(int3(p, mip)).r;
}

float3 safe_inverse(float3 d) {
    return float3(d.x != 0.0 ? 1.0 / d.x : SSR_FAR, d.y != 0.0 ? 1.0 / d.y : SSR_FAR,
                  d.z != 0.0 ? 1.0 / d.z : SSR_FAR);
}

// Hierarchical min-Z march from `origin` along `dir` (screen uv + depth).
float3 hiz_march(float3 origin, float3 dir, uint max_steps, int max_mip, out bool hit) {
    float2 screen = g_consts.screen.xy;
    float3 inv_dir = safe_inverse(dir);
    int mip = 0;
    float2 res = screen;
    float2 uv_offset = 0.005 / screen;
    uv_offset = float2(dir.x < 0.0 ? -uv_offset.x : uv_offset.x,
                       dir.y < 0.0 ? -uv_offset.y : uv_offset.y);
    float2 floor_offset = float2(dir.x < 0.0 ? 0.0 : 1.0, dir.y < 0.0 ? 0.0 : 1.0);

    // Step off the starting cell so the ray does not hit its own pixel.
    float2 plane0 = (floor(res * origin.xy) + floor_offset) / res + uv_offset;
    float2 t0 = plane0 * inv_dir.xy - origin.xy * inv_dir.xy;
    float t = min(t0.x, t0.y);
    float3 pos = origin + t * dir;

    hit = false;
    for (uint i = 0; i < max_steps; ++i) {
        if (any(pos.xy < 0.0) || any(pos.xy > 1.0) || pos.z < 0.0 || pos.z >= 1.0) {
            return pos; // left the screen or the depth range: a miss
        }
        float2 mip_pos = res * pos.xy;
        float surface_z = load_hiz(mip_pos, mip);
        float2 plane = (floor(mip_pos) + floor_offset) / res + uv_offset;
        float3 tb = float3(plane, surface_z) * inv_dir - origin * inv_dir;
        tb.z = dir.z > 0.0 ? tb.z : SSR_FAR; // only a receding ray meets the plane ahead
        float t_min = min(min(tb.x, tb.y), tb.z);
        bool above = surface_z > pos.z;
        bool skipped = asuint(t_min) != asuint(tb.z) && above;
        t = above ? t_min : t;
        pos = origin + t * dir;
        if (skipped) {
            mip = min(mip + 1, max_mip);
        } else {
            --mip;
            if (mip < 0) {
                hit = true;
                return pos;
            }
        }
        res = screen * exp2(-float(mip));
    }
    return pos;
}

float validate_hit(float3 hit, float2 uv, float3 world_dir) {
    float2 screen = g_consts.screen.xy;
    if (any(hit.xy < 0.0) || any(hit.xy > 1.0)) {
        return 0.0;
    }
    if (all(abs(hit.xy - uv) < 2.0 * g_consts.screen.zw)) {
        return 0.0; // did not leave the starting pixel
    }
    int2 texel = clamp(int2(hit.xy * screen), int2(0, 0), int2(screen) - 1);
    float surface_z = g_depth.Load(int3(texel, 0)).r;
    if (surface_z >= 1.0) {
        return 0.0; // the background: the environment is the better answer
    }
    float3 hit_n = oct_decode(g_normal.Load(int3(texel, 0)).xy);
    if (dot(hit_n, world_dir) > 0.0) {
        return 0.0; // the back of a surface
    }
    float3 surface = ssr_world(g_consts.inv_view_proj, hit.xy, surface_z);
    float3 ray = ssr_world(g_consts.inv_view_proj, hit.xy, hit.z);
    float confidence = 1.0 - smoothstep(0.0, g_consts.params.y, length(surface - ray));
    confidence *= confidence;
    float2 fade = max(g_consts.params.z * float2(screen.y / screen.x, 1.0), 1e-4);
    float2 border = smoothstep(0.0, fade, hit.xy) * (1.0 - smoothstep(1.0 - fade, 1.0, hit.xy));
    return confidence * border.x * border.y;
}

[shader("compute")]
[numthreads(8, 8, 1)]
void compute_main(uint3 tid : SV_DispatchThreadID) {
    uint2 size = uint2(g_consts.screen.xy);
    if (tid.x >= size.x || tid.y >= size.y) {
        return;
    }
    int2 coord = int2(tid.xy);
    float4 mat = g_material.Load(int3(coord, 0));
    float roughness = mat.g;
    if (mat.a < 0.5 || roughness > g_consts.params.x) {
        g_hits[coord] = float4(0.0, 0.0, 0.0, 0.0);
        return;
    }
    float2 uv = (float2(coord) + 0.5) * g_consts.screen.zw;
    float depth = g_depth.Load(int3(coord, 0)).r;
    float3 p = ssr_world(g_consts.inv_view_proj, uv, depth);
    float3 v = normalize(g_consts.camera_pos.xyz - p);
    float3 n = oct_decode(g_normal.Load(int3(coord, 0)).xy);
    float3 t;
    float3 b;
    ssr_basis(n, t, b);
    float3 ve = float3(dot(v, t), dot(v, b), dot(v, n));
    if (ve.z <= 1e-4) {
        g_hits[coord] = float4(0.0, 0.0, 0.0, 0.0);
        return;
    }
    float a = max(roughness * roughness, 1e-3);
    float3 ht = ssr_sample_vndf(ve, a, ssr_random2(float2(coord), g_consts.camera_pos.w));
    float3 h = t * ht.x + b * ht.y + n * ht.z;
    float3 l = reflect(-v, h);
    if (dot(l, n) <= 0.0) {
        g_hits[coord] = float4(0.0, 0.0, 0.0, 0.0);
        return;
    }
    float pdf = ssr_vndf_pdf(ve.z, ht.z, a);

    // A second point on the ray, kept in front of the near plane.
    float w0 = ssr_mul(g_consts.view_proj, float4(p, 1.0)).w;
    float dw = ssr_mul(g_consts.view_proj, float4(l, 0.0)).w;
    float s = dw < 0.0 ? 0.5 * w0 / -dw : max(w0, 1.0);
    float4 end = ssr_project(g_consts.view_proj, p + l * s);
    float3 origin = float3(uv, depth);
    float3 dir = end.xyz - origin;

    bool hit;
    float3 pos = hiz_march(origin, dir, uint(g_consts.params.w), int(g_consts.params2.x) - 1, hit);
    float confidence = hit ? validate_hit(pos, uv, l) : 0.0;
    g_hits[coord] = float4(pos.xy, pdf, confidence);
}
