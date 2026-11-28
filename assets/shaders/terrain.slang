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
// Shading is the layered terrain material (R26.1, lib/terrain_material.slang)
// through ITerrainMaterial: texture-set layers from the cooked texture arrays
// (ArrayLayerSource; an untextured layer falls back to its tinted colour with
// procedural detail), placed by slope, altitude, the per-tile curvature /
// flow attributes (terrain_attributes.slang) and the edit paint channels.
// Writes the same GBuffer layout as gbuffer.slang; the deferred lighting
// pass shades terrain like any mesh.

import velocity;
import terrain_grid;
import terrain_material;

struct PushConstants {
    float4x4 view_proj;      // jittered
    float4   chunk;  // x,y = chunk origin (world xz), z = chunk size m, w = skirt depth m
    float4   region; // x,y = heightmap uv offset, z = uv scale, w = metres per texel
    float4   shade;  // x = grid_n, y = heightmap texel uv, z = snowline m, w unused
    float4   misc;   // x = tile origin world y, yzw = camera world position
    float4x4 prev_view_proj; // last frame, unjittered (terrain is static: camera motion only)
    float4   motion;         // xy = this frame's jitter (NDC), zw unused
}
[[vk::push_constant]] PushConstants pc;

[[vk::binding(0)]] Texture2D    g_heightmap; // R32F, metres
[[vk::binding(1)]] SamplerState g_sampler;   // linear clamp
// Edit paint (G19 terrain edits): r dirt, g gravel, b garden soil weights,
// a grass-density mask (read by the grass pass, not here). A 1x1 "unpainted"
// texel for tiles without edits.
[[vk::binding(2)]] Texture2D    g_paint;
// Per-tile attributes (terrain_attributes.slang): r = concavity * 0.5 + 0.5,
// g = flow (0 dry .. 1 water collects).
[[vk::binding(3)]] Texture2D    g_attributes;
[[vk::binding(4)]] ConstantBuffer<TerrainMaterialParams> g_material;
// Layer texture sets, one array slice per textured set: sRGB albedo, X/Y
// normal (RG), packed R roughness / G occlusion / B height.
[[vk::binding(5)]] Texture2DArray g_albedo;
[[vk::binding(6)]] Texture2DArray g_normal;
[[vk::binding(7)]] Texture2DArray g_packed;
[[vk::binding(8)]] SamplerState   g_wrap; // trilinear + anisotropic, repeat

struct VsOut {
    float4 clip      : SV_Position;
    float3 world     : WORLD_POS;
    float2 uv        : TEXCOORD0; // heightmap uv
    float4 cur_clip  : CUR_CLIP;
    float4 prev_clip : PREV_CLIP;
}

// Vertex id -> chunk grid coordinate (lib/terrain_grid.slang).
void decode_vertex(uint id, out float2 g, out bool skirt) {
    terrain_decode_vertex(id, uint(pc.shade.x), g, skirt);
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
    o.cur_clip = o.clip;
    o.prev_clip = mul(pc.prev_view_proj, float4(o.world, 1.0));
    return o;
}

// The cooked texture arrays; untextured layers (slice < 0) are their tinted
// colour with procedural metre-scale detail.
struct ArrayLayerSource : ITerrainLayerSource {
    float4 global(uint index) {
        if (index == 0) return g_material.global0;
        if (index == 1) return g_material.global1;
        float4 g = g_material.global2;
        g.y = pc.shade.z; // this frame's snowline
        return g;
    }

    TerrainLayerGpu layer(uint index) { return g_material.layers[index]; }

    LayerTexel sample(TerrainLayerGpu l, float2 uv, float2 duvdx, float2 duvdy) {
        LayerTexel t;
        if (l.surface.w < 0.0) {
            const float d = terrain_fbm(uv * 2.0);
            t.albedo = l.fallback.rgb * (0.8 + 0.4 * d);
            t.normal_xy = float2(0.0, 0.0);
            t.roughness = l.fallback.w;
            t.occlusion = 1.0;
            t.height = d;
            return t;
        }
        const float3 a = float3(uv, l.surface.w);
        t.albedo = g_albedo.SampleGrad(g_wrap, a, duvdx, duvdy).rgb;
        t.normal_xy = g_normal.SampleGrad(g_wrap, a, duvdx, duvdy).rg * 2.0 - 1.0;
        const float3 p = g_packed.SampleGrad(g_wrap, a, duvdx, duvdy).rgb;
        t.roughness = p.r;
        t.occlusion = p.g;
        t.height = p.b;
        return t;
    }

    LayerTexel mean(TerrainLayerGpu l) {
        LayerTexel t;
        t.normal_xy = float2(0.0, 0.0);
        if (l.surface.w < 0.0) {
            t.albedo = l.fallback.rgb;
            t.roughness = l.fallback.w;
            t.occlusion = 1.0;
            t.height = 0.5;
            return t;
        }
        const float3 a = float3(0.5, 0.5, l.surface.w);
        const float lod = g_material.global2.z; // the 1x1 mip
        t.albedo = g_albedo.SampleLevel(g_wrap, a, lod).rgb;
        const float3 p = g_packed.SampleLevel(g_wrap, a, lod).rgb;
        t.roughness = p.r;
        t.occlusion = p.g;
        t.height = p.b;
        return t;
    }
}

struct GBufferOut {
    float4 albedo    : SV_Target0;
    float2 normal    : SV_Target1;
    float4 material  : SV_Target2; // r metallic, g roughness, b occlusion, a mask
    float2 clearcoat : SV_Target3;
    float4 emissive  : SV_Target4; // terrain emits nothing
    float2 velocity  : SV_Target5;
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

// Shading of one ground point by material M.
TerrainSurface shade<M : ITerrainMaterial>(M material, VsOut input) {
    // World normal from heightmap central differences (LOD-independent — the
    // full-resolution field shades even the coarsest chunks).
    const float du = pc.shade.y;
    const float mpt = pc.region.w;
    const float hl = g_heightmap.SampleLevel(g_sampler, input.uv - float2(du, 0.0), 0.0).r;
    const float hr = g_heightmap.SampleLevel(g_sampler, input.uv + float2(du, 0.0), 0.0).r;
    const float hd = g_heightmap.SampleLevel(g_sampler, input.uv - float2(0.0, du), 0.0).r;
    const float hu = g_heightmap.SampleLevel(g_sampler, input.uv + float2(0.0, du), 0.0).r;
    const float3 n = normalize(float3(hl - hr, 2.0 * mpt, hd - hu));

    const float2 attr = g_attributes.SampleLevel(g_sampler, input.uv, 0.0).rg;
    TerrainSurfaceInput s;
    s.world_pos = input.world;
    s.normal = n;
    s.slope_deg = degrees(acos(saturate(n.y)));
    s.concavity = attr.r * 2.0 - 1.0;
    s.flow = attr.g;
    s.paint = g_paint.SampleLevel(g_sampler, input.uv, 0.0);
    s.view_distance = length(input.world - pc.misc.yzw);
    s.dpdx = ddx(input.world);
    s.dpdy = ddy(input.world);
    return material.evaluate(s);
}

[shader("fragment")]
GBufferOut fragment_main(VsOut input) {
    LayeredTerrainMaterial<ArrayLayerSource> material;
    material.source = ArrayLayerSource();
    const TerrainSurface surf = shade(material, input);

    GBufferOut o;
    o.albedo = float4(surf.albedo, 1.0);
    o.normal = oct_encode(surf.normal);
    o.material = float4(0.0, surf.roughness, surf.occlusion, 1.0);
    o.clearcoat = float2(0.0, 0.0);
    o.emissive = float4(0.0, 0.0, 0.0, 0.0);
    o.velocity = surface_velocity(input.cur_clip, input.prev_clip, pc.motion.xy);
    return o;
}
