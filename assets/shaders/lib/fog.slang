// fog.slang — froxel volumetric fog (ROADMAP R13.3).
//
// Wronski, "Volumetric Fog: Unified Compute Shader Based Solution to
// Atmospheric Scattering" (SIGGRAPH 2014 Advances) and Hillaire,
// "Physically Based and Unified Volumetric Rendering in Frostbite"
// (SIGGRAPH 2015 Advances): participating media are voxelised into a
// camera-aligned froxel grid (FOG_X x FOG_Y screen tiles x FOG_Z depth
// slices), lit per froxel (sun through the cascaded shadow map and the cloud
// shadow map, R10.1's clustered point / spot lights, the sky's ambient),
// filtered over time with a jittered sample position, then integrated
// front to back so any pixel reads its in-scattering and transmittance with
// one trilinear fetch at its distance.
//
// Media: an exponential height fog (constant below its base height) plus
// local fog volumes (box / ellipsoid, soft edges, their own density and
// albedo; scene FogVolume components). Coefficients are physical (1/m),
// scattering = extinction x albedo.
//
// Slices are spaced logarithmically with an offset, depth(w) = o ((far / o
// + 1)^w - 1) for w in [0, 1] (Unreal's volumetric-fog distribution): fine
// near the camera, never collapsing at distance 0. Depth is the distance
// along the view ray (m).

import atmosphere;

public static const uint FOG_X = 160;
public static const uint FOG_Y = 90;
public static const uint FOG_Z = 64;
public static const float FOG_DEPTH_OFFSET_M = 2.0;
public static const uint FOG_MAX_VOLUMES = 64;

// Per-frame fog values (std140, 304 bytes; engine::renderer::FogFrameGpu).
public struct FogFrame {
    public float4 inv_view_proj[4];  // clip -> world (render jitter removed), glm columns
    public float4 prev_view_proj[4]; // last frame world -> clip (render jitter removed)
    public float4 camera;            // xyz world (m), w far distance of the grid (m)
    public float4 prev_camera;       // xyz last frame's camera (m), w history valid (0/1)
    public float4 height_fog;        // x extinction /m at base, y falloff /m, z base height m, w anisotropy g
    public float4 albedo;            // rgb scattering albedo, w temporal blend (current weight)
    public float4 sun;               // xyz unit toward the light, w shadows (0 none, 1 CSM)
    public float4 sun_radiance;      // rgb light radiance (scene units), w cloud shadows (0/1)
    public float4 ambient;           // rgb flat ambient radiance, w local volume count
    public float4 jitter;            // xyz froxel jitter (0..1), w IBL intensity (0 = no cube)
    public float4 cloud_shadow_u;    // CloudShadowProjection
    public float4 cloud_shadow_v;
    public float4 atmosphere;        // x: 1 = the light reaches the fog through the
                                     // transmittance LUT (as the lit surfaces do)
}

// One local fog volume (std430, 80 bytes; engine::renderer::FogVolumeGpu):
// rows of the world -> unit-volume transform (the box spans [-1, 1]³, the
// ellipsoid is the unit sphere).
public struct FogVolumeGpu {
    public float4 row0;
    public float4 row1;
    public float4 row2;
    public float4 albedo_density; // rgb albedo, w extinction /m
    public float4 params;         // x shape (0 box, 1 ellipsoid), y edge fade (0..1), z height falloff /unit, w unused
}

public float fog_depth(float w, float far_m) {
    return FOG_DEPTH_OFFSET_M * (pow(far_m / FOG_DEPTH_OFFSET_M + 1.0, w) - 1.0);
}

public float fog_w(float depth_m, float far_m) {
    return log2(max(depth_m, 0.0) / FOG_DEPTH_OFFSET_M + 1.0) /
           log2(far_m / FOG_DEPTH_OFFSET_M + 1.0);
}

// Extinction of the exponential height fog at height y.
public float height_fog_extinction(float4 height_fog, float y) {
    return height_fog.x * exp(-height_fog.y * max(y - height_fog.z, 0.0));
}

// Extinction (/m) of one local volume at world position `p`.
public float fog_volume_density(FogVolumeGpu v, float3 p) {
    float4 hp = float4(p, 1.0);
    float3 q = float3(dot(v.row0, hp), dot(v.row1, hp), dot(v.row2, hp));
    float inside = v.params.x > 0.5 ? 1.0 - length(q) : 1.0 - max(abs(q.x), max(abs(q.y), abs(q.z)));
    if (inside <= 0.0) return 0.0;
    float fade = v.params.y > 0.0 ? saturate(inside / v.params.y) : 1.0;
    float height = exp(-v.params.z * max(q.y + 1.0, 0.0));
    return v.albedo_density.w * fade * height;
}

// The integrated volume at screen uv and distance `depth_m`: rgb in-scattered
// radiance, a transmittance. Texel z holds the integral to the far boundary
// of slice z; in front of slice 0's boundary the value fades from "no fog".
public float4 sample_fog(Texture3D volume, SamplerState samp, float2 uv, float depth_m, float far_m) {
    float w = fog_w(min(depth_m, far_m), far_m);
    float tc = w - 0.5 / float(FOG_Z);
    float4 v = volume.SampleLevel(samp, float3(uv, max(tc, 0.5 / float(FOG_Z))), 0.0);
    float w0 = 1.0 / float(FOG_Z);
    if (w < w0) v = lerp(float4(0.0, 0.0, 0.0, 1.0), v, saturate(w / w0));
    return v;
}

// The sky's isotropic ambient radiance from the diffuse irradiance cube
// (which stores irradiance / pi, the radiance a white Lambertian surface
// reflects): the mean of the upper and lower hemispheres.
public float3 fog_ambient(TextureCube irradiance, SamplerState samp, float3 flat, float intensity) {
    if (intensity <= 0.0) return flat;
    float3 up = irradiance.SampleLevel(samp, float3(0.0, 1.0, 0.0), 0.0).rgb;
    float3 down = irradiance.SampleLevel(samp, float3(0.0, -1.0, 0.0), 0.0).rgb;
    return flat + 0.5 * (up + down) * intensity;
}

// The exponential height fog's optical depth along a ray from `origin`
// (height y0) in direction `dir` over [t0, t1] (m), in closed form (the
// usual analytic height fog; heights below the base are taken at the base).
public float height_fog_optical_depth(float4 height_fog, float y0, float dir_y, float t0, float t1) {
    if (height_fog.x <= 0.0 || t1 <= t0) return 0.0;
    float k = height_fog.y;
    float y_start = y0 + dir_y * t0;
    float base = height_fog_extinction(height_fog, y_start);
    float slope = k * dir_y;
    float len = t1 - t0;
    if (abs(slope) * len < 1e-4) return base * len;
    if (y_start < height_fog.z && dir_y < 0.0) return base * len; // under the base, going down
    return base * (1.0 - exp(-slope * len)) / slope;
}

// The sun's transmittance through the air down to the camera's view height
// (0 below the horizon): the tint the lighting pass gives the sun on
// surfaces, so lit fog matches lit ground at dawn and dusk.
public float3 fog_sun_transmittance(Texture2D lut, SamplerState samp, float mu) {
    return intersects_ground(ATMOS_VIEW_HEIGHT, mu)
               ? float3(0.0, 0.0, 0.0)
               : sample_transmittance(lut, samp, ATMOS_VIEW_HEIGHT, mu);
}

public float fog_hg_phase(float cos_theta, float g) {
    float g2 = g * g;
    return (1.0 - g2) / (4.0 * 3.14159265358979 * pow(max(1.0 + g2 - 2.0 * g * cos_theta, 1e-6), 1.5));
}
