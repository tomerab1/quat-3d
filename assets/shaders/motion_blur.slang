// motion_blur.slang — motion blur, step 3 of 3 (R20): reconstruction.
//
// McGuire, Hennessy, Bukowski and Osman 2012, "A Reconstruction Filter for
// Plausible Motion Blur", with the sampling of Guertin, McGuire and
// Nowrouzezahrai 2014 / Jimenez 2014: per display pixel, samples spread along
// the tile's dominant (neighbour-max) half vector — every other sample along
// the pixel's own vector instead, so a moving object over a differently
// moving background blurs along both — with interleaved-gradient jitter.
// Each sample Y contributes to pixel X when
//   * Y is in front of X and Y's own blur covers X (cone of Y's velocity),
//   * X is in front of Y and X's blur covers Y (cone of X's velocity), or
//   * both are blurred and cover each other (the cylinder term);
// depth order is a soft comparison over a fraction of the depth. The pixel
// itself weighs 1 / |v_X|. Pixels whose neighbourhood barely moves are
// copied unchanged.

import post_common;
import motion_blur_common;

Texture2D         g_color;    // display extent, linear HDR
Texture2D<float2> g_velocity; // render extent, UV motion over a frame
Texture2D         g_depth;    // render extent, [0, 1]
Texture2D<float2> g_neighbor; // tile grid, dominant half vector (px)
[format("rgba16f")] RWTexture2D<float4> g_out;

[[vk::push_constant]] MotionBlurParams pc;

float depth_at(int2 p) {
    const int2 t = render_texel(p, pc.sizes.zw / pc.sizes.xy, int2(pc.sizes.zw));
    return linear_depth(g_depth.Load(int3(t, 0)).r, pc.proj.xy);
}

// 1 when depth a is in front of depth b, fading over the soft extent.
float soft_depth_compare(float a, float b, float extent) {
    return saturate(1.0 - (a - b) / extent);
}

float cone(float dist, float len) {
    return saturate(1.0 - dist / len);
}

float cylinder(float dist, float len) {
    return 1.0 - smoothstep(0.95 * len, 1.05 * len, dist);
}

[shader("compute")]
[numthreads(8, 8, 1)]
void compute_main(uint3 tid : SV_DispatchThreadID) {
    const int2 p = int2(tid.xy);
    const int2 display = int2(pc.sizes.xy);
    if (any(p >= display)) return;
    const float3 center = g_color.Load(int3(p, 0)).rgb;
    const int2 tile = min(p / int(pc.blur.y), int2(pc.proj.zw) - 1);
    const float2 vn = g_neighbor.Load(int3(tile, 0));
    const float vn_len = length(vn);
    if (vn_len < 0.5) {
        g_out[p] = float4(center, 1.0);
        return;
    }

    const float2 vx = blur_half_vector(pc, g_velocity, p);
    const float vx_len = max(length(vx), 0.5);
    const float zx = depth_at(p);
    const float2 vx_dir = length(vx) >= 0.5 ? vx : vn;
    const int samples = clamp(int(pc.blur.z), 2, 64);
    const float jitter = interleaved_gradient_noise(float2(p) + pc.blur.w * 5.588238) - 0.5;

    float  weight = 1.0 / vx_len;
    float3 sum = center * weight;
    for (int i = 0; i < samples; ++i) {
        // Symmetric positions in (-1, 1), jittered by up to half a step.
        const float t = lerp(-1.0, 1.0, (float(i) + jitter + 1.0) / float(samples + 1));
        const float2 dir = (i & 1) == 0 ? vn : vx_dir;
        const int2 y = clamp(int2(round(float2(p) + dir * t)), int2(0, 0), display - 1);
        if (all(y == p)) continue;
        const float dist = length(float2(y - p));
        const float zy = depth_at(y);
        const float vy_len = max(length(blur_half_vector(pc, g_velocity, y)), 0.5);
        const float extent = max(pc.depth.x * min(zx, zy), 1e-3);
        const float front = soft_depth_compare(zy, zx, extent); // Y in front of X
        const float back = soft_depth_compare(zx, zy, extent);  // X in front of Y
        const float a = front * cone(dist, vy_len) + back * cone(dist, vx_len) +
                        cylinder(dist, vy_len) * cylinder(dist, vx_len) * 2.0;
        weight += a;
        sum += a * g_color.Load(int3(y, 0)).rgb;
    }
    g_out[p] = float4(sum / weight, 1.0);
}
