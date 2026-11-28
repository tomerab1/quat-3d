// motion_blur_neighbor.slang — motion blur, step 2 of 3 (R20): neighbour max.
//
// Each tile takes the longest tile-max vector of its 3 x 3 neighbourhood
// (McGuire et al. 2012, NeighborMax): a blur can reach at most one tile
// (its radius is clamped to the tile size), so this is the dominant motion
// that may cover any pixel of the tile. Diagonal neighbours count only when
// their vector points toward this tile (Jimenez 2014), which keeps fast
// diagonal motion from widening the blur where it cannot reach.

import motion_blur_common;

Texture2D<float2> g_tiles;
[format("rg16f")] RWTexture2D<float2> g_neighbor;

[[vk::push_constant]] MotionBlurParams pc;

[shader("compute")]
[numthreads(8, 8, 1)]
void compute_main(uint3 tid : SV_DispatchThreadID) {
    const int2 tile = int2(tid.xy);
    const int2 grid = int2(pc.proj.zw);
    if (any(tile >= grid)) return;
    float2 best = 0.0;
    for (int dy = -1; dy <= 1; ++dy) {
        for (int dx = -1; dx <= 1; ++dx) {
            const int2 t = tile + int2(dx, dy);
            if (any(t < int2(0, 0)) || any(t >= grid)) continue;
            const float2 v = g_tiles.Load(int3(t, 0));
            if (dx != 0 && dy != 0) {
                // A diagonal tile's blur reaches this one only along the diagonal.
                const float2 toward = -float2(dx, dy);
                if (abs(dot(normalize(v + 1e-6), normalize(toward))) < 0.7) continue;
            }
            if (dot(v, v) > dot(best, best)) best = v;
        }
    }
    g_neighbor[tile] = best;
}
