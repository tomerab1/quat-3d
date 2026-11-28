// motion_blur_common.slang — parameters and the blur vector shared by the
// motion blur passes (R20; MotionBlurPass in motion_blur_pass.cpp).
//
// The velocity target (R11, engine/renderer/velocity.hpp) holds the UV
// motion over the last frame at the render extent. The blur is that motion
// times the shutter fraction (shutter angle / 360) in display pixels; the
// passes work with HALF vectors (the blur extends +-v around a pixel, as in
// McGuire et al. 2012), clamped to the tile size.
//
// Velocity and depth are jittered render-extent targets. Each display pixel
// takes both from the closest-depth sample of its render footprint
// (depth_footprint.slang, issue #115): a moving silhouette keeps its own
// motion and depth in every frame of the jitter cycle (Jimenez 2014 dilates
// the velocity the same way). motion_blur_tiles resolves that once per
// display pixel into display-extent images the reconstruction reads.

import post_common;
import depth_footprint;

// Mirrors MotionBlurPushConstants in motion_blur_pass.cpp (80 bytes).
public struct MotionBlurParams {
    public float4 sizes;  // xy display extent, zw render extent
    public float4 blur;   // x shutter fraction, y tile size = max radius (px), z samples, w frame
    public float4 proj;   // x P[2][2], y P[3][2], zw tile grid size
    public float4 depth;  // x soft depth extent (fraction of depth), yzw unused
    public float4 jitter; // xy this frame's jitter (render px), zw unused
}

// Half blur vector (display pixels) of a UV motion over a frame.
public float2 blur_half_vector(MotionBlurParams pc, float2 velocity_uv) {
    float2 v = velocity_uv * pc.sizes.xy * (0.5 * pc.blur.x);
    if (!all(isfinite(v))) v = 0.0;
    const float len = length(v);
    return len > pc.blur.y ? v * (pc.blur.y / len) : v;
}

// One display pixel's blur input: half vector (display px) and linear depth.
public struct MotionPixel {
    public float2 half_vector;
    public float  depth;
}

// The blur input of display pixel `p`, from the closest-depth render sample.
public MotionPixel motion_pixel(MotionBlurParams pc, Texture2D<float2> velocity, Texture2D depth,
                                int2 p) {
    const ClosestSample s =
        closest_depth_at_display_pixel(depth, p, pc.sizes.xy, pc.sizes.zw, pc.jitter.xy);
    MotionPixel out;
    out.half_vector = blur_half_vector(pc, velocity.Load(int3(s.texel, 0)));
    out.depth = linear_depth(s.depth, pc.proj.xy);
    return out;
}
