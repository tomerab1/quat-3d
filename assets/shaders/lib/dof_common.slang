// dof_common.slang — parameters and the circle of confusion shared by the
// depth-of-field passes (R20; DofPass in dof_pass.cpp).
//
// CoC convention: a signed RADIUS, negative in front of the focus plane
// (near field), positive behind it (far field). The thin-lens CoC on the
// display is scale + bias / z for a view distance z (PhysicalCamera's
// coc_params), clamped to +-max_radius. The half-resolution passes store it
// in half-resolution pixels (display radius / 2).

import post_common;

// Mirrors DofPushConstants in dof_pass.cpp (64 bytes).
public struct DofParams {
    public float4 sizes;   // xy display extent, zw render extent
    public float4 coc;     // x scale (display px), y bias (display px * m), z max radius (display px), w rings
    public float4 proj;    // x P[2][2], y P[3][2], zw tile grid size
    public float4 half_size; // xy half-resolution extent, zw 1 / half extent
}

// Half-resolution pixels per CoC tile (each tile covers 16 x 16 display px).
public static const int DOF_TILE = 8;

// Signed CoC radius in display pixels of depth `d` (the [0, 1] depth value).
public float coc_radius(DofParams p, float d) {
    const float z = max(linear_depth(d, p.proj.xy), 1e-4);
    return clamp(p.coc.x + p.coc.y / z, -p.coc.z, p.coc.z);
}

// The CoC of the depth under display pixel `pixel`.
public float coc_at_display_pixel(DofParams p, Texture2D depth, int2 pixel) {
    const int2 t = render_texel(pixel, p.sizes.zw / p.sizes.xy, int2(p.sizes.zw));
    return coc_radius(p, depth.Load(int3(t, 0)).r);
}
