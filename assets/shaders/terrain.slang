// terrain.slang — heightmap terrain into the deferred GBuffer (Phase 12, 12.2).
//
// Vertex-texture-fetch geometry: there is no vertex buffer at all. The vertex
// id maps to a (grid_n + 1)² chunk grid (plus a perimeter skirt ring whose ids
// follow the grid's), the height comes from sampling the R32F heightmap, and
// per-chunk placement arrives in push constants. Every LOD's index buffer
// references the same full-resolution vertex ids, so positions are exact at
// every LOD and skirts (dropped by skirt_depth_m) hide the cracks between
// neighbouring chunks at different LODs.
//
// Shading is fully procedural: slope/height splat between grass, rock and
// snow, broken up by triplanar FBM detail (sin-free hash — NVIDIA fast-trig
// collapses frac(sin()) lattices). Writes the same GBuffer layout as
// gbuffer.slang; the deferred lighting pass shades terrain like any mesh.

struct PushConstants {
    float4x4 view_proj;
    float4   chunk;  // x,y = chunk origin (world xz), z = chunk size m, w = skirt depth m
    float4   region; // x,y = heightmap uv offset, z = uv scale, w = metres per texel
    float4   shade;  // x = grid_n, y = heightmap texel uv, z = snowline m, w = rock slope
    float4   misc;   // x = tile origin world y, yzw unused
}
[[vk::push_constant]] PushConstants pc;

[[vk::binding(0)]] Texture2D    g_heightmap; // R32F, metres
[[vk::binding(1)]] SamplerState g_sampler;   // linear clamp

struct VsOut {
    float4 clip     : SV_Position;
    float3 world    : WORLD_POS;
    float2 uv       : TEXCOORD0; // heightmap uv
}

// Vertex id -> chunk grid coordinate. Ids [0, (n+1)²) are the grid; the next
// 4n ids walk the perimeter ring once more for the skirt.
void decode_vertex(uint id, out float2 g, out bool skirt) {
    const uint n = uint(pc.shade.x);
    const uint grid_count = (n + 1) * (n + 1);
    if (id < grid_count) {
        g = float2(float(id % (n + 1)), float(id / (n + 1)));
        skirt = false;
        return;
    }
    skirt = true;
    uint r = id - grid_count;
    const uint side = r / n;
    const float t = float(r % n);
    const float fn = float(n);
    if (side == 0)      g = float2(t, 0.0);        // -z edge, left to right
    else if (side == 1) g = float2(fn, t);          // +x edge
    else if (side == 2) g = float2(fn - t, fn);     // +z edge, right to left
    else                g = float2(0.0, fn - t);    // -x edge
}

[shader("vertex")]
VsOut vertex_main(uint id : SV_VertexID) {
    float2 g;
    bool skirt;
    decode_vertex(id, g, skirt);
    const float2 local01 = g / pc.shade.x;
    const float2 uv = pc.region.xy + local01 * pc.region.z;
    float h = pc.misc.x + g_heightmap.SampleLevel(g_sampler, uv, 0.0).r;
    if (skirt) h -= pc.chunk.w;

    VsOut o;
    o.world = float3(pc.chunk.x + local01.x * pc.chunk.z, h,
                     pc.chunk.y + local01.y * pc.chunk.z);
    o.uv = uv;
    o.clip = mul(pc.view_proj, float4(o.world, 1.0));
    return o;
}

// --------------------------------------------------------------------------
// Procedural detail noise (sin-free hash12 + rotated-octave FBM).
// --------------------------------------------------------------------------
float detail_hash(float2 p) {
    float3 q = frac(float3(p.x, p.y, p.x) * 0.1031);
    q += dot(q, float3(q.y, q.z, q.x) + 33.33);
    return frac((q.x + q.y) * q.z);
}

float detail_noise(float2 p) {
    float2 i = floor(p);
    float2 f = frac(p);
    float2 u = f * f * (3.0 - 2.0 * f);
    float a = detail_hash(i);
    float b = detail_hash(i + float2(1, 0));
    float c = detail_hash(i + float2(0, 1));
    float d = detail_hash(i + float2(1, 1));
    return lerp(lerp(a, b, u.x), lerp(c, d, u.x), u.y);
}

float detail_fbm(float2 p) {
    const float2x2 rot = float2x2(0.80, 0.60, -0.60, 0.80);
    float v = 0.0;
    float amp = 0.5;
    for (int i = 0; i < 4; ++i) {
        v += amp * detail_noise(p);
        p = mul(rot, p) * 2.13 + float2(7.3, 3.1);
        amp *= 0.5;
    }
    return v;
}

// Triplanar FBM: blend the three world-plane projections by the normal.
float triplanar_fbm(float3 wp, float3 n, float freq) {
    float3 w = abs(n);
    w /= (w.x + w.y + w.z);
    return detail_fbm(wp.zy * freq) * w.x + detail_fbm(wp.xz * freq) * w.y +
           detail_fbm(wp.xy * freq) * w.z;
}

float2 sign_nonzero(float2 v) {
    return float2(v.x >= 0.0 ? 1.0 : -1.0, v.y >= 0.0 ? 1.0 : -1.0);
}

float2 oct_encode(float3 n) {
    n /= (abs(n.x) + abs(n.y) + abs(n.z));
    float2 e = n.xy;
    if (n.z < 0.0) {
        e = (1.0 - abs(float2(e.y, e.x))) * sign_nonzero(e);
    }
    return e;
}

struct GBufferOut {
    float4 albedo    : SV_Target0;
    float2 normal    : SV_Target1;
    float4 material  : SV_Target2; // r metallic, g roughness, b occlusion, a mask
    float2 clearcoat : SV_Target3;
}

[shader("fragment")]
GBufferOut fragment_main(VsOut input) {
    // World normal from heightmap central differences (LOD-independent — the
    // full-resolution field shades even the coarsest chunks).
    const float du = pc.shade.y;
    const float mpt = pc.region.w;
    const float hl = g_heightmap.SampleLevel(g_sampler, input.uv - float2(du, 0.0), 0.0).r;
    const float hr = g_heightmap.SampleLevel(g_sampler, input.uv + float2(du, 0.0), 0.0).r;
    const float hd = g_heightmap.SampleLevel(g_sampler, input.uv - float2(0.0, du), 0.0).r;
    const float hu = g_heightmap.SampleLevel(g_sampler, input.uv + float2(0.0, du), 0.0).r;
    const float3 n = normalize(float3(hl - hr, 2.0 * mpt, hd - hu));

    // Procedural splat: grass on flat ground, rock on slopes, snow up high.
    const float detail = triplanar_fbm(input.world, n, 0.15);    // metre-scale
    const float macro = triplanar_fbm(input.world, n, 0.011);    // ~100 m patches

    const float3 grass = lerp(float3(0.10, 0.17, 0.06), float3(0.16, 0.22, 0.09), macro);
    const float3 rock = lerp(float3(0.21, 0.19, 0.17), float3(0.30, 0.27, 0.24), detail);
    const float3 snow = float3(0.83, 0.86, 0.90);

    const float rock_w =
        smoothstep(pc.shade.w + 0.12, pc.shade.w - 0.12, n.y + (detail - 0.5) * 0.10);
    const float snow_w = smoothstep(pc.shade.z - 12.0, pc.shade.z + 12.0,
                                    input.world.y + (macro - 0.5) * 30.0) *
                         smoothstep(0.55, 0.75, n.y);

    float3 albedo = lerp(grass, rock, rock_w);
    albedo = lerp(albedo, snow, snow_w);
    albedo *= 0.85 + 0.3 * detail; // break up tiling

    float roughness = lerp(lerp(0.95, 0.90, rock_w), 0.55, snow_w);

    GBufferOut o;
    o.albedo = float4(albedo, 1.0);
    o.normal = oct_encode(n);
    o.material = float4(0.0, roughness, 1.0, 1.0);
    o.clearcoat = float2(0.0, 0.0);
    return o;
}
