// cloud_temporal.slang — temporal accumulation of the quarter-resolution
// clouds (ROADMAP R13.4, VolumetricCloudPass).
//
// Reprojection as in Schneider 2015 and Hillaire 2016: each texel's cloud
// point (its ray at the march's transmittance-weighted depth) is moved back
// by the wind drift of the last frame and projected with last frame's
// view-projection; the history found there is clamped to the current 3x3
// neighbourhood's colour box (Karis, "High Quality Temporal Supersampling",
// SIGGRAPH 2014 Advances — variance clipping, Salvi 2016) and blended with
// the current jittered march. Static views converge to the mean of many
// jitter offsets; disocclusions and off-screen history fall back to the
// current frame. The cloud depth is accumulated alongside (composite and
// next frame's reprojection use it).

static const float CLOUD_NO_DEPTH = 1.0e4;

// std140; engine::renderer::CloudTemporalGpu.
struct CloudTemporal {
    float4 inv_view_proj[4];  // this frame, clip -> world (render jitter removed)
    float4 prev_view_proj[4]; // last frame, world -> clip (render jitter removed)
    float4 camera;            // xyz world position (m), w history valid (0/1)
    float4 drift;             // xy cloud drift since last frame (m, world xz), z blend, w unused
    float4 extent;            // xy cloud extent
}
ConstantBuffer<CloudTemporal> g_temporal;

Texture2D    g_current;       // rgba16f (march output)
Texture2D    g_current_depth; // r16f (km)
Texture2D    g_history;       // rgba16f (last frame's output)
Texture2D    g_history_depth; // r16f (km)
SamplerState g_sampler;       // bilinear, clamp
[format("rgba16f")] RWTexture2D<float4> g_out;
[format("r16f")] RWTexture2D<float>     g_out_depth;

float4 mul_cols4(float4 cols[4], float4 v) {
    return cols[0] * v.x + cols[1] * v.y + cols[2] * v.z + cols[3] * v.w;
}

[shader("compute")]
[numthreads(8, 8, 1)]
void compute_main(uint3 tid : SV_DispatchThreadID) {
    int2 size = int2(g_temporal.extent.xy);
    if (int(tid.x) >= size.x || int(tid.y) >= size.y) return;
    int2 coord = int2(tid.xy);
    float4 cur = g_current.Load(int3(coord, 0));
    float cur_depth = g_current_depth.Load(int3(coord, 0)).r;

    // Neighbourhood statistics (mean and standard deviation) of the current
    // march, for variance clipping of the history.
    float4 m1 = float4(0.0, 0.0, 0.0, 0.0);
    float4 m2 = float4(0.0, 0.0, 0.0, 0.0);
    float near_depth = cur_depth;
    for (int y = -1; y <= 1; ++y) {
        for (int x = -1; x <= 1; ++x) {
            int2 c = clamp(coord + int2(x, y), int2(0, 0), size - 1);
            float4 v = g_current.Load(int3(c, 0));
            m1 += v;
            m2 += v * v;
            near_depth = min(near_depth, g_current_depth.Load(int3(c, 0)).r);
        }
    }
    m1 /= 9.0;
    float4 sigma = sqrt(max(m2 / 9.0 - m1 * m1, 0.0));
    float4 box_min = m1 - 1.25 * sigma;
    float4 box_max = m1 + 1.25 * sigma;

    float4 result = cur;
    float result_depth = cur_depth;
    if (g_temporal.camera.w > 0.5) {
        // The cloud point this texel sees (the nearest neighbourhood depth:
        // reproject edges with the cloud in front), moved back by the drift.
        float2 uv = (float2(coord) + 0.5) / float2(size);
        float4 far_h = mul_cols4(g_temporal.inv_view_proj, float4(uv * 2.0 - 1.0, 1.0, 1.0));
        float4 near_h = mul_cols4(g_temporal.inv_view_proj, float4(uv * 2.0 - 1.0, 0.5, 1.0));
        float3 dir = normalize(far_h.xyz / far_h.w - near_h.xyz / near_h.w);
        float dist_m = min(near_depth, CLOUD_NO_DEPTH) * 1000.0;
        float3 world = g_temporal.camera.xyz + dir * dist_m;
        world.xz -= g_temporal.drift.xy;
        float4 clip = mul_cols4(g_temporal.prev_view_proj, float4(world, 1.0));
        if (clip.w > 0.0) {
            float2 prev_uv = clip.xy / clip.w * 0.5 + 0.5;
            if (all(prev_uv >= 0.0) && all(prev_uv <= 1.0)) {
                float4 hist = g_history.SampleLevel(g_sampler, prev_uv, 0.0);
                float hist_depth = g_history_depth.SampleLevel(g_sampler, prev_uv, 0.0).r;
                hist = clamp(hist, box_min, box_max);
                float alpha = g_temporal.drift.z;
                result = lerp(hist, cur, alpha);
                result_depth = lerp(hist_depth, cur_depth, alpha);
            }
        }
    }
    g_out[coord] = max(result, float4(0.0, 0.0, 0.0, 0.0));
    g_out_depth[coord] = result_depth;
}
