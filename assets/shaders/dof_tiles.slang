// dof_tiles.slang — depth of field, step 2 of 5 (R20): CoC tile bounds.
//
// One thread per tile of DOF_TILE x DOF_TILE half-resolution pixels writes
// the tile's largest near-field radius (r) and largest far-field radius (g),
// both >= 0 in half-resolution pixels. The gather sizes its kernel from
// these after dilation (dof_dilate.slang).

import dof_common;

Texture2D g_half; // rgb colour, a signed CoC (half px)
[format("rg16f")] RWTexture2D<float2> g_tiles;

[[vk::push_constant]] DofParams pc;

[shader("compute")]
[numthreads(8, 8, 1)]
void compute_main(uint3 tid : SV_DispatchThreadID) {
    const int2 tile = int2(tid.xy);
    if (any(tile >= int2(pc.proj.zw))) return;
    const int2 half_max = int2(pc.half_size.xy) - 1;
    float near_r = 0.0;
    float far_r = 0.0;
    for (int y = 0; y < DOF_TILE; ++y) {
        for (int x = 0; x < DOF_TILE; ++x) {
            const int2 p = min(tile * DOF_TILE + int2(x, y), half_max);
            const float coc = g_half.Load(int3(p, 0)).a;
            near_r = max(near_r, -coc);
            far_r = max(far_r, coc);
        }
    }
    g_tiles[tile] = float2(near_r, far_r);
}
