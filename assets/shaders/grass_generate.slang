// grass_generate.slang — per-frame grass blade generation + culling (R9.3).
//
// One thread per candidate cell of the camera-centred grid, one 8x8 tile of
// cells per workgroup. A tile entirely outside the radius or the view
// frustum exits before touching memory. Each surviving cell hashes its world
// cell index (blades never move with the camera), pulls its root toward its
// clump centre (grass v2), samples the terrain height and normal, applies
// the slope / height limits, the distance LOD and the pixel-footprint LOD
// (density thinning with a height fade before a blade drops out, survivors
// widened to keep coverage), tests the blade's bounding sphere against the
// frustum, records the ground's albedo under the root (root tint), and
// appends it to the near- or far-LOD list. Appends are aggregated per workgroup in shared
// memory: one global atomic per list per tile.
//
// finalize_main (one thread, after generation) clamps the raw counts to the
// list capacities and writes the two VkDrawIndexedIndirectCommands.

import grass_common;

[[vk::binding(0)]] ConstantBuffer<GrassParams>  g_params;
[[vk::binding(1)]] RWStructuredBuffer<GrassBlade> g_blades;
[[vk::binding(2)]] RWStructuredBuffer<uint>     g_args;
[[vk::binding(3)]] SamplerState                 g_sampler; // linear, clamp to edge
[[vk::binding(4)]] Texture2D                    g_height0; // R32F terrain tiles (metres)
[[vk::binding(5)]] Texture2D                    g_height1;
[[vk::binding(6)]] Texture2D                    g_height2;
[[vk::binding(7)]] Texture2D                    g_height3;
[[vk::binding(8)]] Texture2D                    g_albedo;  // GBuffer albedo (root tint)
[[vk::binding(9)]] Texture2D                    g_depth;   // GBuffer depth

static const uint TILE = 8;

groupshared uint s_count[4]; // near, far, placed, candidates
groupshared uint s_base[2];

bool ground_at(float2 xz, out float height, out float3 normal) {
    return grass_ground_at(g_params, g_height0, g_height1, g_height2, g_height3, g_sampler, xz,
                           height, normal);
}

// The ground's albedo under a blade's root, from the GBuffer the opaque
// passes (mesh, terrain, ground cover) wrote this frame, so the blade's base
// blends into whatever it stands on (Ghost of Tsushima's root tint). Packed
// as unorm8 sqrt(rgb) + weight; weight 0 where the root is off screen or
// hidden behind something (the GBuffer there is not the ground under it).
uint ground_tint(float3 root, float dist) {
    if (g_params.look.w < 0.5) return 0;
    const float4 clip = mul(g_params.view_proj, float4(root, 1.0));
    if (clip.w <= 1e-4) return 0;
    const float2 uv = clip.xy / clip.w * 0.5 + 0.5;
    if (any(uv < 0.0) || any(uv >= 1.0)) return 0;
    const int2 px = int2(uv * g_params.screen.xy);
    const float depth = g_depth.Load(int3(px, 0)).r;
    const float4 h = mul(g_params.inv_view_proj,
                         float4((float2(px) + 0.5) * g_params.screen.zw * 2.0 - 1.0, depth, 1.0));
    const float3 seen = h.xyz / h.w;
    if (length(seen - root) > 0.06 + 0.01 * dist) return 0;
    const float3 albedo = sqrt(saturate(g_albedo.Load(int3(px, 0)).rgb));
    const uint3 q = uint3(albedo * 255.0 + 0.5);
    return q.x | (q.y << 8u) | (q.z << 16u) | (255u << 24u);
}

bool sphere_visible(float3 c, float r) {
    for (uint i = 0; i < 6; ++i) {
        float4 pl = g_params.planes[i];
        if (dot(pl.xyz, c) + pl.w < -r) return false;
    }
    return true;
}

bool box_visible(float3 mn, float3 mx) {
    for (uint i = 0; i < 6; ++i) {
        float4 pl = g_params.planes[i];
        float3 v = float3(pl.x >= 0.0 ? mx.x : mn.x, pl.y >= 0.0 ? mx.y : mn.y,
                          pl.z >= 0.0 ? mx.z : mn.z);
        if (dot(pl.xyz, v) + pl.w < 0.0) return false;
    }
    return true;
}

[shader("compute")]
[numthreads(8, 8, 1)]
void compute_main(uint3 group : SV_GroupID, uint3 thread : SV_GroupThreadID,
                  uint local : SV_GroupIndex) {
    if (local < 4) s_count[local] = 0;
    GroupMemoryBarrierWithGroupSync();

    // Tile culling: uniform per workgroup, so the early exit keeps every
    // later barrier uniform too.
    const float cell = g_params.cell.x;
    const float radius = g_params.cell.y;
    const float3 cam = g_params.camera.xyz;
    const int2 tile_cell = g_params.grid.xy + int2(group.xy) * int(TILE);
    const float2 tile_min = float2(tile_cell) * cell;
    const float2 tile_max = tile_min + float(TILE) * cell;
    const float2 nearest = clamp(cam.xz, tile_min, tile_max);
    // Roots move up to clump.z toward their clump centre: grow the bounds.
    const float pull = g_params.clump.z;
    const float3 box_min = float3(tile_min.x - pull, g_params.ground.x, tile_min.y - pull);
    const float3 box_max =
        float3(tile_max.x + pull, g_params.ground.y + g_params.lod.z, tile_max.y + pull);
    if (length(nearest - cam.xz) > radius + pull) return;
    if (!box_visible(box_min, box_max)) return;

    // The candidate of this world cell (grass_candidate on the CPU).
    const int2 world_cell = tile_cell + int2(thread.xy);
    const uint3 h = pcg3d(uint3(uint(world_cell.x), uint(world_cell.y), g_params.counts.x));
    const float3 u = unit_floats(h);
    float2 root_xz = (float2(world_cell) + u.xy) * cell;
    const float threshold = u.z;
    const uint hash = (h.x ^ h.z) & 0xFFFFu;
    uint clump_hash = 0;
    if (g_params.clump.x > 0.0) {
        const Clump clump = grass_clump(g_params, root_xz);
        root_xz += (clump.centre - root_xz) * g_params.clump.y;
        clump_hash = clump.hash;
    }

    bool candidate = length(root_xz - cam.xz) < radius;
    bool placed = false;
    bool visible = false;
    uint lod = 0;
    GrassBlade blade;
    blade.root = float3(root_xz.x, 0.0, root_xz.y);
    blade.packed = 0;
    blade.clump = clump_hash;
    blade.tint = 0;
    blade.reserved = uint2(0, 0);
    float dist = 0.0;
    if (candidate) {
        float ground_y;
        float3 n;
        if (ground_at(root_xz, ground_y, n) && grass_ground_allowed(g_params, ground_y, n)) {
            blade.root.y = ground_y;
            dist = length(blade.root - cam);
            const float footprint = grass_footprint_keep(g_params, dist);
            const float fade = grass_blade_fade(
                grass_keep(g_params, dist) * grass_slope_keep(g_params, n.y) * footprint,
                threshold);
            if (fade > 0.0) {
                placed = true;
                const BladeShape shape = blade_shape(g_params, hash, clump_hash);
                const float height = shape.height * fade;
                visible = sphere_visible(blade.root + float3(0.0, 0.5 * height, 0.0), height);
                const float width_scale =
                    grass_width_scale(g_params, grass_thinning(g_params, dist) * footprint);
                blade.packed = pack_blade(hash, fade, width_scale, g_params.lod.w);
                lod = dist < g_params.cell.z ? 0 : 1;
            }
        }
    }
    if (visible) blade.tint = ground_tint(blade.root, dist);

    uint slot = 0;
    if (candidate) InterlockedAdd(s_count[3], 1u);
    if (placed) InterlockedAdd(s_count[2], 1u);
    if (visible) InterlockedAdd(s_count[lod], 1u, slot);
    GroupMemoryBarrierWithGroupSync();
    if (local == 0) {
        uint base = 0;
        if (s_count[0] > 0) InterlockedAdd(g_args[ARGS_NEAR_COUNT], s_count[0], base);
        s_base[0] = base;
        base = 0;
        if (s_count[1] > 0) InterlockedAdd(g_args[ARGS_FAR_COUNT], s_count[1], base);
        s_base[1] = base;
        if (s_count[2] > 0) InterlockedAdd(g_args[ARGS_PLACED], s_count[2]);
        if (s_count[3] > 0) InterlockedAdd(g_args[ARGS_CANDIDATES], s_count[3]);
    }
    GroupMemoryBarrierWithGroupSync();
    if (visible) {
        const uint index = s_base[lod] + slot;
        if (lod == 0) {
            if (index < g_params.counts.z) g_blades[index] = blade;
        } else if (index < g_params.counts.w) {
            g_blades[g_params.counts.z + index] = blade;
        }
    }
}

[shader("compute")]
[numthreads(1, 1, 1)]
void finalize_main() {
    const uint near_cap = g_params.counts.z;
    const uint near = min(g_args[ARGS_NEAR_COUNT], near_cap);
    const uint far = min(g_args[ARGS_FAR_COUNT], g_params.counts.w);
    g_args[ARGS_NEAR + 0] = g_params.draw.x; // indexCount
    g_args[ARGS_NEAR + 1] = near;            // instanceCount
    g_args[ARGS_NEAR + 2] = 0;               // firstIndex
    g_args[ARGS_NEAR + 3] = 0;               // vertexOffset
    g_args[ARGS_NEAR + 4] = 0;               // firstInstance
    g_args[ARGS_FAR + 0] = g_params.draw.y;
    g_args[ARGS_FAR + 1] = far;
    g_args[ARGS_FAR + 2] = g_params.draw.x;  // far indices follow the near ones
    g_args[ARGS_FAR + 3] = 0;
    g_args[ARGS_FAR + 4] = 0;                // firstInstance: the region base is a push constant
                                             // (drawIndirectFirstInstance is not required)
}
