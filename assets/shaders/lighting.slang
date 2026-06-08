// lighting.slang — deferred PBR lighting pass (Phase 3 Slice 3.6; PBR in 4.5).
//
// A fullscreen compute pass: reads the GBuffer (albedo, octahedral world normal,
// packed metallic/roughness/occlusion, depth) and shades one directional light
// with a Cook-Torrance microfacet BRDF (GGX NDF, height-correlated Smith
// visibility, Schlick Fresnel) plus a flat ambient term, writing linear HDR
// colour. World position is reconstructed from depth via the inverse view-proj,
// giving the per-pixel view vector specular needs. The material target's alpha is
// the coverage mask (1 where geometry was rasterised, 0 for background).

static const float PI = 3.14159265358979;

// Light + camera parameters, pushed as a 128-byte block (matches
// LightingPushConstants in lighting_pass.cpp).
struct LightParams {
    float4x4 inv_view_proj; // reconstruct world pos from depth
    float4   camera_pos;    // xyz = world-space camera position
    float4   direction;     // xyz = direction TO the light (normalised)
    float4   color;         // rgb = colour, w = intensity
    float4   ambient;       // rgb = ambient colour, w unused
}
[[vk::push_constant]] LightParams pc;

[[vk::binding(0)]] Texture2D          g_albedo;
[[vk::binding(1)]] Texture2D          g_normal;
[[vk::binding(2)]] Texture2D          g_material;
[[vk::binding(3)]] Texture2D          g_depth;
[[vk::binding(4)]] RWTexture2D<float4> g_hdr;

// Octahedral decode (inverse of gbuffer.slang's oct_encode).
float3 oct_decode(float2 e) {
    float3 n = float3(e.x, e.y, 1.0 - abs(e.x) - abs(e.y));
    float t = max(-n.z, 0.0);
    n.x += n.x >= 0.0 ? -t : t;
    n.y += n.y >= 0.0 ? -t : t;
    return normalize(n);
}

// Reconstruct world position from the pixel's screen UV + sampled depth. The
// inverse of the same view_proj used to rasterise (which already carries the
// Vulkan clip-space Y flip), so no manual flip here.
float3 world_from_depth(int2 coord, uint width, uint height, float depth) {
    float2 uv  = (float2(coord) + 0.5) / float2(width, height);
    float2 ndc = uv * 2.0 - 1.0;
    float4 clip = float4(ndc, depth, 1.0);
    float4 world = mul(pc.inv_view_proj, clip);
    return world.xyz / world.w;
}

// GGX / Trowbridge-Reitz normal distribution. `a` = roughness^2.
float d_ggx(float ndoth, float a) {
    float a2 = a * a;
    float d = ndoth * ndoth * (a2 - 1.0) + 1.0;
    return a2 / (PI * d * d);
}

// Height-correlated Smith visibility (already folds in 1 / (4·NdotV·NdotL)).
float v_smith(float ndotv, float ndotl, float a) {
    float a2 = a * a;
    float gv = ndotl * sqrt(ndotv * ndotv * (1.0 - a2) + a2);
    float gl = ndotv * sqrt(ndotl * ndotl * (1.0 - a2) + a2);
    return 0.5 / max(gv + gl, 1e-5);
}

float3 f_schlick(float vdoth, float3 f0) {
    return f0 + (1.0 - f0) * pow(1.0 - vdoth, 5.0);
}

[shader("compute")]
[numthreads(8, 8, 1)]
void compute_main(uint3 tid : SV_DispatchThreadID) {
    uint width = 0;
    uint height = 0;
    g_hdr.GetDimensions(width, height);
    if (tid.x >= width || tid.y >= height) {
        return;
    }
    int2 coord = int2(tid.xy);

    float4 mat = g_material.Load(int3(coord, 0));
    if (mat.a < 0.5) { // background: no geometry was rasterised here
        g_hdr[coord] = float4(0.0, 0.0, 0.0, 1.0);
        return;
    }

    float4 albedo = g_albedo.Load(int3(coord, 0));
    float2 enc    = g_normal.Load(int3(coord, 0)).xy;
    float  depth  = g_depth.Load(int3(coord, 0)).r;

    float metallic  = mat.r;
    float roughness = mat.g;
    float occlusion = mat.b;

    float3 n = oct_decode(enc);
    float3 world_pos = world_from_depth(coord, width, height, depth);
    float3 v = normalize(pc.camera_pos.xyz - world_pos);
    float3 l = normalize(pc.direction.xyz);
    float3 h = normalize(v + l);

    float ndotl = max(dot(n, l), 0.0);
    float ndotv = max(dot(n, v), 1e-4);
    float ndoth = max(dot(n, h), 0.0);
    float vdoth = max(dot(v, h), 0.0);

    float  a  = max(roughness * roughness, 1e-3);
    float3 f0 = lerp(float3(0.04), albedo.rgb, metallic);

    float  D   = d_ggx(ndoth, a);
    float  Vis = v_smith(ndotv, ndotl, a);
    float3 F   = f_schlick(vdoth, f0);

    float3 spec    = D * Vis * F;
    float3 kd      = (1.0 - F) * (1.0 - metallic);
    float3 diffuse = kd * albedo.rgb / PI;

    float3 radiance = pc.color.rgb * pc.color.w;
    float3 lit      = (diffuse + spec) * radiance * ndotl;
    float3 ambient  = pc.ambient.rgb * albedo.rgb * occlusion;

    g_hdr[coord] = float4(lit + ambient, 1.0);
}
