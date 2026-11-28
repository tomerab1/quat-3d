// fog_scatter.slang — froxel fog: media, lighting and temporal filter
// (ROADMAP R13.3, FogPass; lib/fog.slang). One thread per froxel:
//   1. the sample point, jittered inside the froxel by a per-frame offset
//      (Hillaire 2015: the temporal filter turns the jitter into
//      supersampling);
//   2. media at the point: the exponential height fog plus every local fog
//      volume (extinction /m, albedo);
//   3. light scattered toward the camera: the scene's directional light
//      through the cascaded shadow map and the cloud shadow map, R10.1's
//      clustered point / spot lights (the froxel's cluster list), and the
//      sky's isotropic ambient, with a Henyey-Greenstein phase function;
//   4. reprojection into last frame's grid and an exponential blend with
//      the history found there.
// Output (history ping-pong): rgb = scattering coefficient x in-scattered
// radiance (per metre), a = extinction (/m).

import fog;
import csm;
import clustered_lights;
import cloud_shadow_common;

ConstantBuffer<FogFrame>            g_fog;
StructuredBuffer<FogVolumeGpu>      g_volumes;
Texture2DArray                      g_shadow_map;
SamplerComparisonState              g_shadow_cmp;
StructuredBuffer<CascadeData>       g_cascades;
Texture2D                           g_cloud_shadow;
SamplerState                        g_linear; // bilinear, clamp
ConstantBuffer<ClusterParams>       g_cluster_params;
StructuredBuffer<GpuLight>          g_lights;
StructuredBuffer<uint2>             g_cluster_grid;
StructuredBuffer<uint>              g_cluster_indices;
TextureCube                         g_irradiance;
Texture2D                           g_transmittance; // atmosphere LUT (atmosphere.x)
Texture3D                           g_history;
[format("rgba16f")] RWTexture3D<float4> g_out;

float4 mul_cols4(float4 cols[4], float4 v) {
    return cols[0] * v.x + cols[1] * v.y + cols[2] * v.z + cols[3] * v.w;
}

float3 ray_dir(float2 uv) {
    float2 ndc = uv * 2.0 - 1.0;
    float4 a = mul_cols4(g_fog.inv_view_proj, float4(ndc, 1.0, 1.0));
    float4 b = mul_cols4(g_fog.inv_view_proj, float4(ndc, 0.5, 1.0));
    return normalize(a.xyz / a.w - b.xyz / b.w);
}

[shader("compute")]
[numthreads(8, 8, 1)]
void compute_main(uint3 tid : SV_DispatchThreadID) {
    if (tid.x >= FOG_X || tid.y >= FOG_Y || tid.z >= FOG_Z) return;
    float far_m = g_fog.camera.w;
    float3 cell = float3(tid) + g_fog.jitter.xyz;
    float2 uv = cell.xy / float2(FOG_X, FOG_Y);
    float3 dir = ray_dir(uv);
    float dist = fog_depth(cell.z / float(FOG_Z), far_m);
    float3 p = g_fog.camera.xyz + dir * dist;

    // Media.
    float sigma_t = height_fog_extinction(g_fog.height_fog, p.y);
    float3 scatter = sigma_t * g_fog.albedo.rgb;
    uint count = min(uint(g_fog.ambient.w), FOG_MAX_VOLUMES);
    for (uint i = 0; i < count; ++i) {
        FogVolumeGpu v = g_volumes[i];
        float e = fog_volume_density(v, p);
        sigma_t += e;
        scatter += e * v.albedo_density.rgb;
    }

    // Light toward the camera.
    float3 radiance = fog_ambient(g_irradiance, g_linear, g_fog.ambient.rgb, g_fog.jitter.w);
    float g = g_fog.height_fog.w;
    if (any(g_fog.sun_radiance.rgb > 0.0) && any(scatter > 0.0)) {
        float vis = 1.0;
        if (g_fog.sun.w > 0.5) {
            // The cascades are picked by view depth (clip w).
            float view_depth = dist * dot(dir, ray_dir(float2(0.5, 0.5)));
            vis = csm_visibility_volume(g_cascades[0], g_shadow_map, g_shadow_cmp, p, view_depth);
        }
        if (g_fog.sun_radiance.w > 0.5) {
            CloudShadowProjection proj;
            proj.u_axis = g_fog.cloud_shadow_u;
            proj.v_axis = g_fog.cloud_shadow_v;
            vis *= cloud_shadow_sample(g_cloud_shadow, g_linear, proj, p);
        }
        float3 sun_rad = g_fog.sun_radiance.rgb;
        if (g_fog.atmosphere.x > 0.5) {
            sun_rad *= fog_sun_transmittance(g_transmittance, g_linear, g_fog.sun.y);
        }
        radiance += sun_rad * (vis * fog_hg_phase(dot(dir, g_fog.sun.xyz), g));
    }
    if (g_cluster_params.dims.w > 0u && any(scatter > 0.0)) {
        uint2 range = g_cluster_grid[cluster_index_of_world(g_cluster_params, p)];
        for (uint k = 0; k < range.y; ++k) {
            GpuLight light = g_lights[g_cluster_indices[range.x + k]];
            float3 to_light;
            float3 l;
            if (light_incident(light, p, to_light, l)) {
                radiance += l * fog_hg_phase(dot(dir, to_light), g);
            }
        }
    }
    float4 current = float4(scatter * radiance, sigma_t);

    // Temporal filter: where this sample was in last frame's grid.
    float4 result = current;
    if (g_fog.prev_camera.w > 0.5) {
        float4 clip = mul_cols4(g_fog.prev_view_proj, float4(p, 1.0));
        if (clip.w > 0.0) {
            float2 prev_uv = clip.xy / clip.w * 0.5 + 0.5;
            float prev_w = fog_w(length(p - g_fog.prev_camera.xyz), far_m);
            if (all(prev_uv >= 0.0) && all(prev_uv <= 1.0) && prev_w <= 1.0) {
                float4 hist = g_history.SampleLevel(g_linear, float3(prev_uv, prev_w), 0.0);
                result = lerp(hist, current, g_fog.albedo.w);
            }
        }
    }
    g_out[tid] = max(result, float4(0.0, 0.0, 0.0, 0.0));
}
