// post_common.slang — helpers shared by the display-extent post passes
// (R20 depth of field and motion blur), which read the render-extent depth
// and velocity targets after TAAU has resolved the colour to the display
// extent. Those reads go through the closest-depth footprint
// (depth_footprint.slang), never the nearest jittered texel.

// Linear view distance (> 0) of a [0, 1] depth with the projection's
// P[2][2] (x) and P[3][2] (y): z_view = -P32 / (d + P22).
public float linear_depth(float d, float2 proj) {
    return proj.y / (d + proj.x);
}

// Interleaved gradient noise (Jimenez 2014) in [0, 1).
public float interleaved_gradient_noise(float2 pixel) {
    return frac(52.9829189 * frac(dot(pixel, float2(0.06711056, 0.00583715))));
}
