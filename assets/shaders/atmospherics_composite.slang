// atmospherics_composite.slang — applies the participating media to the lit
// HDR image (ROADMAP R13.3 / R13.4, AtmosphericsPass), one thread per
// pixel at the render extent, between the lighting pass and the
// transparents. From the surface (or the sky) toward the camera:
//   1. aerial perspective (opaque pixels; Hillaire 2020's froxel volume at
//      the pixel's distance) — the sky already holds its own;
//   2. the volumetric clouds (quarter resolution, temporally accumulated,
//      upsampled bilinearly; only where the clouds' depth is in front of
//      the pixel);
//   3. the exponential height fog beyond the froxel grid's range, in closed
//      form (lit by the ambient and the unshadowed sun);
//   4. the integrated froxel fog (Wronski 2014 / Hillaire 2015) at the
//      pixel's distance.
// Each step is L = L_behind * T + S. Sky-unit terms (aerial perspective,
// clouds) are scaled into the HDR's units by `scale.x`.

import aerial_perspective;
import atmosphere;
import fog;
import hdr; // hdr_store

// std140, 208 bytes; engine::renderer::AtmosphericsGpu.
struct Atmospherics {
    float4 inv_view_proj[4]; // clip -> world (jittered, as the HDR was rendered)
    float4 camera;           // xyz world (m), w unused
    float4 flags;            // x aerial perspective, y clouds, z froxel fog, w analytic height fog
    float4 scale;            // x sky-unit -> HDR scale (incl. pre-exposure), y pre-exposure, z fog far (m), w IBL intensity
    float4 height_fog;       // as FogFrame.height_fog
    float4 fog_albedo;       // rgb albedo
    float4 sun;              // xyz unit toward the light, w unused
    float4 sun_radiance;     // rgb (scene units)
    float4 ambient;          // rgb flat ambient radiance
    float4 atmosphere;       // x: 1 = the sun reaches the fog through the transmittance LUT
}
ConstantBuffer<Atmospherics> g_comp;

[format("rgba16f")] RWTexture2D<float4> g_hdr;
Texture2D    g_depth;
Texture2D    g_cloud;       // rgb luminance (sky units), a transmittance
Texture2D    g_cloud_depth; // km
Texture3D    g_ap;
Texture3D    g_fog;
TextureCube  g_irradiance;
Texture2D    g_transmittance; // atmosphere LUT (atmosphere.x)
SamplerState g_linear; // bilinear, clamp

float4 mul_cols4(float4 cols[4], float4 v) {
    return cols[0] * v.x + cols[1] * v.y + cols[2] * v.z + cols[3] * v.w;
}

[shader("compute")]
[numthreads(8, 8, 1)]
void compute_main(uint3 tid : SV_DispatchThreadID) {
    uint w = 0;
    uint h = 0;
    g_hdr.GetDimensions(w, h);
    if (tid.x >= w || tid.y >= h) return;
    int2 coord = int2(tid.xy);
    float2 uv = (float2(coord) + 0.5) / float2(w, h);
    float depth = g_depth.Load(int3(coord, 0)).r;
    bool sky = depth >= 1.0;
    float4 far_h = mul_cols4(g_comp.inv_view_proj, float4(uv * 2.0 - 1.0, sky ? 1.0 : depth, 1.0));
    float3 to_p = far_h.xyz / far_h.w - g_comp.camera.xyz;
    float dist = sky ? 1.0e9 : length(to_p);
    float3 dir = normalize(to_p);
    float3 col = g_hdr[coord].rgb;
    float sky_scale = g_comp.scale.x;

    if (g_comp.flags.x > 0.5 && !sky) {
        float4 ap = sample_aerial_perspective(g_ap, g_linear, uv, dist * 0.001);
        col = col * ap.a + ap.rgb * sky_scale;
    }
    if (g_comp.flags.y > 0.5) {
        float cloud_km = g_cloud_depth.SampleLevel(g_linear, uv, 0.0).r;
        if (sky || cloud_km * 1000.0 < dist) {
            float4 c = g_cloud.SampleLevel(g_linear, uv, 0.0);
            col = col * c.a + c.rgb * sky_scale;
        }
    }
    float far_m = g_comp.scale.z;
    if (g_comp.flags.w > 0.5) {
        float t0 = g_comp.flags.z > 0.5 ? far_m : 0.0;
        float t1 = min(dist, 2.0e5);
        float tau = height_fog_optical_depth(g_comp.height_fog, g_comp.camera.y, dir.y, t0, t1);
        if (tau > 0.0) {
            float trans = exp(-tau);
            float3 sun_rad = g_comp.sun_radiance.rgb;
            if (g_comp.atmosphere.x > 0.5) {
                sun_rad *= fog_sun_transmittance(g_transmittance, g_linear, g_comp.sun.y);
            }
            float3 lin = fog_ambient(g_irradiance, g_linear, g_comp.ambient.rgb, g_comp.scale.w) +
                         sun_rad * fog_hg_phase(dot(dir, g_comp.sun.xyz), g_comp.height_fog.w);
            col = col * trans + g_comp.fog_albedo.rgb * lin * (1.0 - trans) * g_comp.scale.y;
        }
    }
    if (g_comp.flags.z > 0.5) {
        float4 f = sample_fog(g_fog, g_linear, uv, dist, far_m);
        col = col * f.a + f.rgb; // pre-exposed by the integration
    }
    g_hdr[coord] = float4(hdr_store(col), 1.0);
}
