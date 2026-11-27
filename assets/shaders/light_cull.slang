// light_cull.slang — clustered light culling compute pass (ROADMAP R10.1).
//
// One thread per cluster (froxel). Each workgroup streams the light list
// through shared memory in batches of CULL_GROUP_SIZE (each thread loads one
// light and transforms its culling sphere to view space once for the whole
// group), and every thread tests the batch against its cluster's view-space
// AABB.
//
// The output is a compact list: g_grid[cluster] = (offset, count) into
// g_indices. A cluster first counts its lights, reserves exactly that many
// slots from the global cursor g_counter[0] with one atomic, then re-tests and
// writes them in ascending light order. Counting first (instead of buffering
// hits in a per-thread array) keeps register/scratch use flat and puts no cap
// on a single cluster; the index list only needs to hold the frame's total.
// When the total exceeds the capacity, clusters reserving past the end keep a
// truncated list (g_counter[0] still holds the requested total for
// diagnostics). g_counter must be zeroed before the dispatch (the pass fills
// it in a preceding graph pass).

import clustered_lights;

static const uint CULL_GROUP_SIZE = 64;

ConstantBuffer<ClusterParams> g_params;
StructuredBuffer<GpuLight>    g_lights;
RWStructuredBuffer<uint2>     g_grid;    // per cluster: offset, count
RWStructuredBuffer<uint>      g_indices; // light indices
RWStructuredBuffer<uint>      g_counter; // [0] = indices requested

groupshared float4 s_spheres[CULL_GROUP_SIZE]; // view-space centre, radius

// Stages lights [base, base + CULL_GROUP_SIZE) into shared memory.
void load_batch(ClusterParams p, uint base, uint local, uint light_count) {
    uint li = base + local;
    float4 sphere = float4(0.0, 0.0, 0.0, 0.0); // radius 0 = touches nothing
    if (li < light_count) {
        GpuLight l = g_lights[li];
        float3 c = cluster_mul(p.view, float4(l.position_range.xyz, 1.0)).xyz;
        sphere = float4(c, l.position_range.w);
    }
    s_spheres[local] = sphere;
}

[shader("compute")]
[numthreads(64, 1, 1)]
void compute_main(uint3 tid : SV_DispatchThreadID, uint local : SV_GroupIndex) {
    ClusterParams p = g_params;
    uint cluster_total = p.dims.x * p.dims.y * p.dims.z;
    uint cluster = tid.x;
    bool active = cluster < cluster_total;
    uint light_count = p.dims.w;

    float3 box_min = float3(0.0, 0.0, 0.0);
    float3 box_max = float3(0.0, 0.0, 0.0);
    if (active) {
        uint3 c = uint3(cluster % p.dims.x, (cluster / p.dims.x) % p.dims.y,
                        cluster / (p.dims.x * p.dims.y));
        cluster_aabb(p, c, box_min, box_max);
    }

    // Pass 1: count. Every thread takes part in the shared-memory batches
    // (inactive ones only load) so the group barriers stay uniform.
    uint count = 0;
    for (uint base = 0; base < light_count; base += CULL_GROUP_SIZE) {
        load_batch(p, base, local, light_count);
        GroupMemoryBarrierWithGroupSync();
        uint n = min(CULL_GROUP_SIZE, light_count - base);
        if (active) {
            for (uint j = 0; j < n; ++j) {
                float4 s = s_spheres[j];
                if (cluster_sphere_overlaps(box_min, box_max, s.xyz, s.w)) {
                    ++count;
                }
            }
        }
        GroupMemoryBarrierWithGroupSync();
    }

    // Reserve exactly `count` slots; clamp to the list capacity.
    uint offset = 0;
    if (active && count > 0) {
        InterlockedAdd(g_counter[0], count, offset);
    }
    uint capacity = p.limits.x;
    uint stored = offset >= capacity ? 0 : min(count, capacity - offset);

    // Pass 2: write the indices (ascending light order). The loop is uniform
    // across the group (barriers inside); the writes are per thread.
    uint written = 0;
    for (uint base = 0; base < light_count; base += CULL_GROUP_SIZE) {
        load_batch(p, base, local, light_count);
        GroupMemoryBarrierWithGroupSync();
        uint n = min(CULL_GROUP_SIZE, light_count - base);
        for (uint j = 0; j < n && written < stored; ++j) {
            float4 s = s_spheres[j];
            if (cluster_sphere_overlaps(box_min, box_max, s.xyz, s.w)) {
                g_indices[offset + written] = base + j;
                ++written;
            }
        }
        GroupMemoryBarrierWithGroupSync();
    }
    if (active) {
        g_grid[cluster] = uint2(offset, stored);
    }
}
