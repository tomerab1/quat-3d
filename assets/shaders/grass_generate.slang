// grass_generate.slang — per-frame grass blade generation + culling (R9.3).
//
// One thread per candidate cell of the camera-centred grid, one 8x8 tile of
// cells per workgroup. A tile entirely outside the radius or the view
// frustum exits before touching memory. Each surviving cell hashes its world
// cell index (blades never move with the camera), samples the terrain
// height and normal, applies the slope / height limits and the distance
// LOD (density thinning with a height fade before a blade drops out), tests
// the blade's bounding sphere against the frustum, and appends it to the
// near- or far-LOD list. Appends are aggregated per workgroup in shared
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

static const uint TILE = 8;

groupshared uint s_count[4]; // near, far, placed, candidates
groupshared uint s_base[2];

float sample_tile(uint tile, float2 uv) {
    switch (tile) {
    case 0: return g_height0.SampleLevel(g_sampler, uv, 0.0).r;
    case 1: return g_height1.SampleLevel(g_sampler, uv, 0.0).r;
    case 2: return g_height2.SampleLevel(g_sampler, uv, 0.0).r;
    default: return g_height3.SampleLevel(g_sampler, uv, 0.0).r;
    }
}

// Ground height + normal at world XZ, with the terrain pass's uv convention
// and central-difference normal. False where there is no ground to grow on.
bool ground_at(float2 xz, out float height, out float3 normal) {
    for (uint k = 0; k < g_params.counts.y; ++k) {
        float4 rect = g_params.tile_rect[k];
        float2 local = xz - rect.xy;
        if (all(local >= 0.0) && all(local <= rect.z)) {
            float2 uv = local / rect.z;
            float du = g_params.tile_info[k].x;
            float mpt = g_params.tile_info[k].y;
            height = rect.w + sample_tile(k, uv);
            float hl = sample_tile(k, uv - float2(du, 0.0));
            float hr = sample_tile(k, uv + float2(du, 0.0));
            float hd = sample_tile(k, uv - float2(0.0, du));
            float hu = sample_tile(k, uv + float2(0.0, du));
            normal = normalize(float3(hl - hr, 2.0 * mpt, hd - hu));
            return true;
        }
    }
    height = g_params.tip_color.w;
    normal = float3(0.0, 1.0, 0.0);
    return g_params.base_color.w < 0.5;
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
    if (length(nearest - cam.xz) > radius) return;
    const float3 box_min = float3(tile_min.x, g_params.ground.x, tile_min.y);
    const float3 box_max = float3(tile_max.x, g_params.ground.y + g_params.lod.z, tile_max.y);
    if (!box_visible(box_min, box_max)) return;

    // The candidate of this world cell (grass_candidate on the CPU).
    const int2 world_cell = tile_cell + int2(thread.xy);
    const uint3 h = pcg3d(uint3(uint(world_cell.x), uint(world_cell.y), g_params.counts.x));
    const float3 u = unit_floats(h);
    const float2 root_xz = (float2(world_cell) + u.xy) * cell;
    const float threshold = u.z;
    const uint hash = (h.x ^ h.z) & 0xFFFFu;

    bool candidate = length(root_xz - cam.xz) < radius;
    bool placed = false;
    bool visible = false;
    uint lod = 0;
    GrassBlade blade;
    blade.root = float3(root_xz.x, 0.0, root_xz.y);
    blade.packed = 0;
    if (candidate) {
        float ground_y;
        float3 n;
        if (ground_at(root_xz, ground_y, n) && n.y >= g_params.limits.x &&
            n.y <= g_params.limits.y && ground_y >= g_params.limits.z &&
            ground_y <= g_params.limits.w) {
            blade.root.y = ground_y;
            const float dist = length(blade.root - cam);
            const float fade = grass_blade_fade(grass_keep(g_params, dist), threshold);
            if (fade > 0.0) {
                placed = true;
                const BladeShape shape = blade_shape(g_params, hash);
                const float height = shape.height * fade;
                visible = sphere_visible(blade.root + float3(0.0, 0.5 * height, 0.0), height);
                const float width_scale =
                    min(rsqrt(max(grass_thinning(g_params, dist), 1e-4)), g_params.lod.w);
                blade.packed = pack_blade(hash, fade, width_scale, g_params.lod.w);
                lod = dist < g_params.cell.z ? 0 : 1;
            }
        }
    }

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
