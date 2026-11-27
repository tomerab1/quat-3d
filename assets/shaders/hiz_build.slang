// hiz_build.slang — hierarchical depth (Hi-Z) pyramid (R12).
//
// Mip 0 is a copy of the depth buffer (both channels); every further texel
// stores the min (r: nearest) and max (g: farthest) depth of its footprint in
// the level above. A level is half the size of its source, rounded down;
// when the source extent is odd the last texel's footprint grows to three
// texels on that axis, so every source texel is covered and the reduction
// stays conservative (occlusion culling reads g, screen-space tracing r).
//
// HiZPass records one dispatch per level after mip 1, with a compute ->
// compute barrier in between. The first dispatch (first = 1) runs over mip
// 1's texels and does both levels at once: it reads each footprint from the
// depth buffer, writes those texels to mip 0 (footprints partition mip 0)
// and their min / max to mip 1. It also reduces the min / max depth of
// geometry pixels (depth < 1; the cleared background is excluded) into
// g_bounds as float bit patterns (non-negative floats order like their
// bits) — the sample-distribution input for cascade fitting (SDSM,
// csm_setup.slang).

struct HiZParams {
    uint2 src_size; // extent of the level read (mip 0 for the first dispatch)
    uint2 dst_size; // extent of the level written (mip 1 for the first dispatch)
    uint  first;    // 1: read g_depth, write mip 0 + mip 1 and the depth bounds
}
[[vk::push_constant]] HiZParams pc;

Texture2D                             g_depth;
[format("rg32f")] RWTexture2D<float2> g_level0; // mip 0 (first dispatch)
[format("rg32f")] RWTexture2D<float2> g_src;
[format("rg32f")] RWTexture2D<float2> g_dst;
RWStructuredBuffer<uint>              g_bounds; // [0] = min depth bits, [1] = max

groupshared uint gs_min;
groupshared uint gs_max;

static const uint FLOAT_INF_BITS = 0x7F800000u;

// The footprint of destination texel `coord` in its source level: origin
// and size (2, or 3 on the last texel of an odd source axis).
void footprint(uint2 coord, out uint2 base, out uint2 count) {
    base = coord * 2u;
    count = uint2(2u, 2u);
    if ((pc.src_size.x & 1u) != 0u && coord.x == pc.dst_size.x - 1u) {
        count.x = 3u;
    }
    if ((pc.src_size.y & 1u) != 0u && coord.y == pc.dst_size.y - 1u) {
        count.y = 3u;
    }
}

void first_levels(uint2 coord, uint group_index) {
    if (group_index == 0) {
        gs_min = FLOAT_INF_BITS;
        gs_max = 0u;
    }
    GroupMemoryBarrierWithGroupSync();
    if (all(coord < pc.dst_size)) {
        uint2 base;
        uint2 count;
        footprint(coord, base, count);
        uint2 last = pc.src_size - 1u;
        float2 result = float2(1.0e30, -1.0e30);
        float geo_min = 1.0e30;
        float geo_max = -1.0;
        for (uint y = 0u; y < count.y; ++y) {
            for (uint x = 0u; x < count.x; ++x) {
                uint2 p = min(base + uint2(x, y), last);
                float d = g_depth.Load(int3(int2(p), 0)).r;
                g_level0[p] = float2(d, d);
                result = float2(min(result.x, d), max(result.y, d));
                if (d < 1.0) {
                    geo_min = min(geo_min, d);
                    geo_max = max(geo_max, d);
                }
            }
        }
        g_dst[coord] = result;
        if (geo_max >= 0.0) {
            InterlockedMin(gs_min, asuint(max(geo_min, 0.0)));
            InterlockedMax(gs_max, asuint(geo_max));
        }
    }
    GroupMemoryBarrierWithGroupSync();
    if (group_index == 0 && gs_min <= gs_max) {
        InterlockedMin(g_bounds[0], gs_min);
        InterlockedMax(g_bounds[1], gs_max);
    }
}

void reduce_level(uint2 coord) {
    if (any(coord >= pc.dst_size)) {
        return;
    }
    uint2 base;
    uint2 count;
    footprint(coord, base, count);
    uint2 last = pc.src_size - 1u;
    float2 result = float2(1.0e30, -1.0e30);
    for (uint y = 0u; y < count.y; ++y) {
        for (uint x = 0u; x < count.x; ++x) {
            float2 v = g_src[min(base + uint2(x, y), last)];
            result = float2(min(result.x, v.x), max(result.y, v.y));
        }
    }
    g_dst[coord] = result;
}

[shader("compute")]
[numthreads(8, 8, 1)]
void compute_main(uint3 tid : SV_DispatchThreadID, uint group_index : SV_GroupIndex) {
    if (pc.first != 0u) {
        first_levels(tid.xy, group_index);
    } else {
        reduce_level(tid.xy);
    }
}
