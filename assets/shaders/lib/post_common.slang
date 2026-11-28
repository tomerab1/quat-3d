// post_common.slang — helpers shared by the display-extent post passes
// (R20 depth of field and motion blur), which read the render-extent depth
// and velocity targets after TAAU has resolved the colour to the display
// extent.

// Linear view distance (> 0) of a [0, 1] depth with the projection's
// P[2][2] (x) and P[3][2] (y): z_view = -P32 / (d + P22).
public float linear_depth(float d, float2 proj) {
    return proj.y / (d + proj.x);
}

// The render-extent texel under a display pixel (nearest; render <= display).
public int2 render_texel(int2 display_pixel, float2 display_to_render, int2 render_size) {
    const int2 t = int2((float2(display_pixel) + 0.5) * display_to_render);
    return clamp(t, int2(0, 0), render_size - 1);
}

// Interleaved gradient noise (Jimenez 2014) in [0, 1).
public float interleaved_gradient_noise(float2 pixel) {
    return frac(52.9829189 * frac(dot(pixel, float2(0.06711056, 0.00583715))));
}
