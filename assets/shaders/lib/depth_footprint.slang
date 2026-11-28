// depth_footprint.slang — the closest-depth render sample around a display
// pixel (R11 TAAU's velocity dilation; R20 depth of field and motion blur,
// issue #115).
//
// The GBuffer is rendered at the render extent with a sub-pixel jitter
// (jitter.hpp): render sample i sits at unjittered position
// (i + 0.5 - jitter) / render in UV, so in "sample coordinates"
//   rpos = uv * render - 0.5 + jitter
// sample i lies at rpos == i. Which scene point the sample nearest a display
// pixel sees changes every frame; reading that one sample, a pixel on a depth
// edge flips between the foreground and the background from frame to frame.
//
// Taking the closest depth (and the velocity stored at that sample) over the
// 3 x 3 render samples around the nearest one instead keeps silhouette pixels
// on the foreground in every frame: the foreground's depth, circle of
// confusion and motion are dilated by about one render texel, and only
// background pixels beyond that see the jitter. The footprint is measured in
// render texels, so it covers the same scene area at any render scale. This
// is the depth dilation of Karis 2014 ("High Quality Temporal Supersampling",
// SIGGRAPH 2014 Advances in Real-Time Rendering), which Jimenez 2014 ("Next
// Generation Post Processing in Call of Duty: Advanced Warfare", SIGGRAPH
// 2014) also applies to the velocity motion blur reads.
//
// Depth convention: [0, 1] with 1 = far, so the closest sample is the
// smallest value.

// A display pixel's position in render sample coordinates (see the header).
public float2 render_sample_position(int2 display_pixel, float2 display_size,
                                     float2 render_size, float2 jitter) {
    const float2 uv = (float2(display_pixel) + 0.5) / display_size;
    return uv * render_size - 0.5 + jitter;
}

// The render sample nearest to sample coordinates `rpos` (may lie outside
// the render extent at its border; closest_depth_3x3 clamps).
public int2 nearest_render_sample(float2 rpos) {
    return int2(floor(rpos + 0.5));
}

public struct ClosestSample {
    public int2  texel; // render texel of the closest depth
    public float depth; // its [0, 1] depth
}

// The closest depth over the 3 x 3 render samples around `centre` (each
// clamped to the render extent). Ties keep the centre, then the first sample
// in row order, so a flat region resolves to the centre sample.
public ClosestSample closest_depth_3x3(Texture2D depth, int2 centre, int2 render_size) {
    const int2 render_max = render_size - 1;
    ClosestSample best;
    best.texel = clamp(centre, int2(0, 0), render_max);
    best.depth = depth.Load(int3(best.texel, 0)).r;
    [unroll]
    for (int dy = -1; dy <= 1; ++dy) {
        [unroll]
        for (int dx = -1; dx <= 1; ++dx) {
            if (dx == 0 && dy == 0) continue;
            const int2 q = clamp(centre + int2(dx, dy), int2(0, 0), render_max);
            const float d = depth.Load(int3(q, 0)).r;
            if (d < best.depth) {
                best.depth = d;
                best.texel = q;
            }
        }
    }
    return best;
}

// The closest-depth render sample in the footprint of a display pixel, for
// passes that run at the display extent after TAAU.
public ClosestSample closest_depth_at_display_pixel(Texture2D depth, int2 display_pixel,
                                                    float2 display_size, float2 render_size,
                                                    float2 jitter) {
    const float2 rpos = render_sample_position(display_pixel, display_size, render_size, jitter);
    return closest_depth_3x3(depth, nearest_render_sample(rpos), int2(render_size));
}
