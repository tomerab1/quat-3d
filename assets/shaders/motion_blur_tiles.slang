// motion_blur_tiles.slang — motion blur, step 1 of 3 (R20): tile max.
//
// One 8 x 8 workgroup per K x K tile of display pixels (K = the largest blur
// radius) finds the tile's longest half blur vector: each thread scans a
// strided subset of the tile, then a shared-memory reduction keeps the
// longest (McGuire et al. 2012, "A Reconstruction Filter for Plausible
// Motion Blur", TileMax). On the way every display pixel's blur input (half
// vector and linear depth of its closest-depth render sample,
// motion_blur_common.slang) is stored for the reconstruction, which reads
// it many times per pixel.

import motion_blur_common;

Texture2D<float2> g_velocity; // render extent, UV motion over a frame, jittered
Texture2D         g_depth;    // render extent, [0, 1], jittered
[format("rg16f")] RWTexture2D<float2> g_tiles;
[format("rg16f")] RWTexture2D<float2> g_vectors;    // display extent, half vector (px)
[format("r32f")] RWTexture2D<float>   g_view_depth; // display extent, linear depth

[[vk::push_constant]] MotionBlurParams pc;

groupshared float2 s_max[64];

float2 longer(float2 a, float2 b) {
    return dot(b, b) > dot(a, a) ? b : a;
}

[shader("compute")]
[numthreads(8, 8, 1)]
void compute_main(uint3 group : SV_GroupID, uint3 local : SV_GroupThreadID, uint index : SV_GroupIndex) {
    const int  tile_size = int(pc.blur.y);
    const int2 origin = int2(group.xy) * tile_size;
    const int2 display = int2(pc.sizes.xy);
    float2 best = 0.0;
    for (int y = int(local.y); y < tile_size; y += 8) {
        for (int x = int(local.x); x < tile_size; x += 8) {
            const int2 p = origin + int2(x, y);
            if (any(p >= display)) continue;
            const MotionPixel m = motion_pixel(pc, g_velocity, g_depth, p);
            g_vectors[p] = m.half_vector;
            g_view_depth[p] = m.depth;
            best = longer(best, m.half_vector);
        }
    }
    s_max[index] = best;
    GroupMemoryBarrierWithGroupSync();
    for (uint stride = 32; stride > 0; stride >>= 1) {
        if (index < stride) s_max[index] = longer(s_max[index], s_max[index + stride]);
        GroupMemoryBarrierWithGroupSync();
    }
    if (index == 0 && all(int2(group.xy) < int2(pc.proj.zw))) g_tiles[group.xy] = s_max[0];
}
