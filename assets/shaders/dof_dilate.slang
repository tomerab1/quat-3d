// dof_dilate.slang — depth of field, step 3 of 5 (R20): tile dilation.
//
// A blurred foreground spreads its CoC over in-focus pixels behind it (near-
// field bleeding), so a tile must gather from every neighbour whose near CoC
// reaches it, not only from its own content. Each tile takes the largest
// near radius of the tiles within the maximum CoC whose radius covers the
// gap to this tile, and the same for far radii (a far tile's blur also
// spills into the neighbouring far field).

import dof_common;

Texture2D<float2> g_tiles; // r near radius, g far radius (half px)
[format("rg16f")] RWTexture2D<float2> g_dilated;

[[vk::push_constant]] DofParams pc;

[shader("compute")]
[numthreads(8, 8, 1)]
void compute_main(uint3 tid : SV_DispatchThreadID) {
    const int2 tile = int2(tid.xy);
    const int2 grid = int2(pc.proj.zw);
    if (any(tile >= grid)) return;
    // Tiles the largest possible half-resolution radius can reach.
    const int reach = min(int(ceil(pc.coc.z * 0.5 / float(DOF_TILE))), 8);
    float2 r = g_tiles.Load(int3(tile, 0));
    for (int dy = -reach; dy <= reach; ++dy) {
        for (int dx = -reach; dx <= reach; ++dx) {
            const int2 t = tile + int2(dx, dy);
            if (any(t < int2(0, 0)) || any(t >= grid)) continue;
            // Half-resolution pixels between the two tiles' nearest edges.
            const float gap = float(max(max(abs(dx), abs(dy)) - 1, 0) * DOF_TILE);
            const float2 n = g_tiles.Load(int3(t, 0));
            if (n.x > gap) r.x = max(r.x, n.x);
            if (n.y > gap) r.y = max(r.y, n.y);
        }
    }
    g_dilated[tile] = r;
}
