// cloud_shadow.slang — the cloud shadow map (R13.1, CloudShadowPass; the
// R13.4 cloud model).
//
// One thread per texel of an orthographic projection along the sun, centred
// on the camera and snapped to whole texels (lib/cloud_shadow_common.slang
// reads it). The texel's line parallel to the sun is taken where it crosses
// the camera's height, and the sun's transmittance through the cloud layer
// along it is integrated with cloud_shadow_transmittance (lib/clouds.slang),
// the density the sky shows. The CPU mirror is CloudShadowFrame::texel_point
// + cloud_reference::shadow_transmittance.

import atmosphere;
import clouds;

// Mirrors engine::renderer::CloudShadowGpu (push constants, 64 bytes).
struct CloudShadowGpu {
    float4 sun;    // xyz unit, toward the sun, elevation-clamped; w texel size m
    float4 right;  // xyz light-space right; w snapped right coordinate of the centre (m)
    float4 up;     // xyz light-space up; w snapped up coordinate of the centre (m)
    float4 camera; // xyz camera world position; w resolution
}
[[vk::push_constant]] CloudShadowGpu pc;

static const int CLOUD_SHADOW_MAP_STEPS = 16;

ConstantBuffer<CloudLayer> g_layer;
Texture3D    g_base;
Texture3D    g_detail;
Texture2D    g_curl;
Texture2D    g_weather;
SamplerState g_noise_sampler;
[format("r16f")] RWTexture2D<float> g_cloud_shadow;

[shader("compute")]
[numthreads(8, 8, 1)]
void compute_main(uint3 tid : SV_DispatchThreadID) {
    uint width = 0;
    uint height = 0;
    g_cloud_shadow.GetDimensions(width, height);
    if (tid.x >= width || tid.y >= height) return;
    float half_res = pc.camera.w * 0.5;
    float lu = pc.right.w + (float(tid.x) + 0.5 - half_res) * pc.sun.w;
    float lv = pc.up.w + (float(tid.y) + 0.5 - half_res) * pc.sun.w;
    float3 s = pc.sun.xyz;
    // The texel's line, on the plane through the camera perpendicular to the
    // sun, then slid along the line to the camera's height.
    float3 on_plane = pc.right.xyz * lu + pc.up.xyz * lv + s * dot(pc.camera.xyz, s);
    float3 start = on_plane - s * ((on_plane.y - pc.camera.y) / s.y);
    // Camera-relative km; the camera stands at the ground of the cloud frame
    // (altitude 0.2 km, ATMOS_VIEW_HEIGHT) like the sky's.
    float3 rel = (start - pc.camera.xyz) * 0.001;
    rel.y += ATMOS_VIEW_HEIGHT - ATMOS_GROUND_RADIUS;
    g_cloud_shadow[int2(tid.xy)] =
        cloud_shadow_transmittance(g_layer, g_base, g_detail, g_curl, g_weather,
                                   g_noise_sampler, rel, s, CLOUD_SHADOW_MAP_STEPS);
}
