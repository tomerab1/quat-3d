// motion_blur_common.slang — parameters and the blur vector shared by the
// motion blur passes (R20; MotionBlurPass in motion_blur_pass.cpp).
//
// The velocity target (R11, engine/renderer/velocity.hpp) holds the UV
// motion over the last frame at the render extent. The blur is that motion
// times the shutter fraction (shutter angle / 360) in display pixels; the
// passes work with HALF vectors (the blur extends +-v around a pixel, as in
// McGuire et al. 2012), clamped to the tile size.

import post_common;

// Mirrors MotionBlurPushConstants in motion_blur_pass.cpp (64 bytes).
public struct MotionBlurParams {
    public float4 sizes;  // xy display extent, zw render extent
    public float4 blur;   // x shutter fraction, y tile size = max radius (px), z samples, w frame
    public float4 proj;   // x P[2][2], y P[3][2], zw tile grid size
    public float4 depth;  // x soft depth extent (fraction of depth), yzw unused
}

// Half blur vector (display pixels) of the velocity under display pixel `p`.
public float2 blur_half_vector(MotionBlurParams pc, Texture2D<float2> velocity, int2 p) {
    const int2 t = render_texel(p, pc.sizes.zw / pc.sizes.xy, int2(pc.sizes.zw));
    float2 v = velocity.Load(int3(t, 0)) * pc.sizes.xy * (0.5 * pc.blur.x);
    if (!all(isfinite(v))) v = 0.0;
    const float len = length(v);
    return len > pc.blur.y ? v * (pc.blur.y / len) : v;
}
