// lighting.slang — deferred PBR lighting pass (Phase 3 Slice 3.6; PBR in 4.5).
//
// A fullscreen compute pass: reads the GBuffer (albedo, octahedral world normal,
// packed metallic/roughness/occlusion, depth) and shades one directional light
// with a Cook-Torrance microfacet BRDF (GGX NDF, height-correlated Smith
// visibility, Schlick Fresnel) plus a flat ambient term, writing linear HDR
// colour. World position is reconstructed from depth via the inverse view-proj,
// giving the per-pixel view vector specular needs. The material target's alpha is
// the coverage mask (1 where geometry was rasterised, 0 for background).

import atmosphere;
import sky;

static const float PI = 3.14159265358979;

// Scales the sky-view LUT's luminance (per-unit sun irradiance) into the
// engine's HDR lighting range.
static const float SKY_LUT_BRIGHTNESS = 8.0;

// Prefiltered specular cubemap mip count - 1 (the highest roughness LOD). Must
// match prefilter_mip_count in ibl_pass.cpp.
static const float PREFILTER_MAX_LOD = 4.0;

// Light + camera parameters, pushed as a 192-byte block (matches
// LightingPushConstants in lighting_pass.cpp).
struct LightParams {
    float4x4 inv_view_proj;   // reconstruct world pos from depth
    float4x4 light_view_proj; // world -> shadow-map clip space
    float4   camera_pos;      // xyz = world-space camera position, w = shadows enabled
    float4   direction;       // xyz = direction TO the light (normalised)
    float4   color;           // rgb = colour, w = intensity
    float4   ambient;         // rgb = ambient colour, w unused
}
[[vk::push_constant]] LightParams pc;

struct PointLight {
    float4 position_radius; // xyz = world position, w = radius
    float4 color_intensity; // rgb = colour, w = intensity
}

[[vk::binding(0)]] Texture2D                  g_albedo;
[[vk::binding(1)]] Texture2D                  g_normal;
[[vk::binding(2)]] Texture2D                  g_material;
[[vk::binding(3)]] Texture2D                  g_depth;
[[vk::binding(4)]] RWTexture2D<float4>        g_hdr;
[[vk::binding(5)]] Texture2D                  g_shadow;
[[vk::binding(6)]] SamplerState               g_shadow_sampler;
[[vk::binding(7)]] StructuredBuffer<PointLight> g_point_lights; // pc.ambient.w entries

// Split-sum IBL maps (precomputed once in ibl_pass.cpp): diffuse irradiance cube,
// prefiltered specular cube (roughness -> mip), and the BRDF integration LUT.
[[vk::binding(8)]]  TextureCube  g_irradiance;
[[vk::binding(9)]]  TextureCube  g_prefiltered;
[[vk::binding(10)]] Texture2D    g_brdf_lut;
[[vk::binding(11)]] SamplerState g_ibl_sampler;
[[vk::binding(12)]] Texture2D    g_clearcoat; // r = coat strength, g = coat roughness
// Atmosphere LUTs (Phase 11.1; bound to fallbacks when pc.direction.w < 2).
[[vk::binding(13)]] Texture2D    g_skyview;
[[vk::binding(14)]] Texture2D    g_transmittance_lut;

// Schlick Fresnel with a roughness term, for the ambient/environment specular
// (the view-independent IBL term, unlike the per-light f_schlick).
float3 fresnel_schlick_roughness(float ndotv, float3 f0, float roughness) {
    float3 fmax = max(float3(1.0 - roughness, 1.0 - roughness, 1.0 - roughness), f0);
    return f0 + (fmax - f0) * pow(saturate(1.0 - ndotv), 5.0);
}

// Image-based ambient: diffuse irradiance + prefiltered specular reflection,
// combined through the split-sum BRDF LUT. Replaces the flat constant ambient.
float3 ibl_ambient(float3 N, float3 V, float3 albedo, float metallic, float roughness,
                   float occlusion) {
    float ndotv = max(dot(N, V), 1e-4);
    float3 f0 = lerp(float3(0.04, 0.04, 0.04), albedo, metallic);
    float3 F = fresnel_schlick_roughness(ndotv, f0, roughness);
    float3 kd = (1.0 - F) * (1.0 - metallic);

    float3 irradiance = g_irradiance.SampleLevel(g_ibl_sampler, N, 0.0).rgb;
    float3 diffuse = irradiance * albedo;

    float3 R = reflect(-V, N);
    float3 prefiltered =
        g_prefiltered.SampleLevel(g_ibl_sampler, R, roughness * PREFILTER_MAX_LOD).rgb;
    float2 brdf = g_brdf_lut.SampleLevel(g_ibl_sampler, float2(ndotv, roughness), 0.0).rg;
    float3 specular = prefiltered * (F * brdf.x + brdf.y);

    return (kd * diffuse + specular) * occlusion;
}

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

// Cook-Torrance response for one light direction L (unit), without the light's
// radiance — returns (diffuse + specular) * NdotL.
float3 surface_brdf(float3 N, float3 V, float3 L, float3 albedo, float metallic, float roughness) {
    float3 h = normalize(V + L);
    float ndotl = max(dot(N, L), 0.0);
    float ndotv = max(dot(N, V), 1e-4);
    float ndoth = max(dot(N, h), 0.0);
    float vdoth = max(dot(V, h), 0.0);

    float  a  = max(roughness * roughness, 1e-3);
    float3 f0 = lerp(float3(0.04), albedo, metallic);

    float  D   = d_ggx(ndoth, a);
    float  Vis = v_smith(ndotv, ndotl, a);
    float3 F   = f_schlick(vdoth, f0);

    float3 spec    = D * Vis * F;
    float3 kd      = (1.0 - F) * (1.0 - metallic);
    float3 diffuse = kd * albedo / PI;
    return (diffuse + spec) * ndotl;
}

// KHR_materials_clearcoat: a second dielectric specular lobe (f0 = 0.04) on top
// of the base material, layered per the glTF spec —
//   coated = base * (1 - cc * Fc) + cc * f_clearcoat
// where Fc is the coat's Schlick fresnel. The coat shares the base normal (coat
// normal maps are not yet supported).

// Coat fresnel used both to weight the coat lobe and attenuate the base layer.
float coat_fresnel(float cosine) {
    return 0.04 + 0.96 * pow(saturate(1.0 - cosine), 5.0);
}

// Punctual-light coat response: layered (base, coat) for one light direction.
float3 layer_clearcoat_direct(float3 base, float3 N, float3 V, float3 L, float cc, float ccr) {
    if (cc <= 0.0) return base;
    float3 h = normalize(V + L);
    float ndotl = max(dot(N, L), 0.0);
    float ndotv = max(dot(N, V), 1e-4);
    float ndoth = max(dot(N, h), 0.0);
    float vdoth = max(dot(V, h), 0.0);
    float ac = max(ccr * ccr, 1e-3);
    float coat = d_ggx(ndoth, ac) * v_smith(ndotv, ndotl, ac) * coat_fresnel(vdoth) * ndotl;
    return base * (1.0 - cc * coat_fresnel(ndotv)) + cc * coat;
}

// Environment coat response: prefiltered reflection at the coat roughness,
// split-sum with the coat f0, layered over the base IBL.
float3 layer_clearcoat_ibl(float3 base, float3 N, float3 V, float cc, float ccr) {
    if (cc <= 0.0) return base;
    float ndotv = max(dot(N, V), 1e-4);
    float3 R = reflect(-V, N);
    float3 prefiltered = g_prefiltered.SampleLevel(g_ibl_sampler, R, ccr * PREFILTER_MAX_LOD).rgb;
    float2 brdf = g_brdf_lut.SampleLevel(g_ibl_sampler, float2(ndotv, ccr), 0.0).rg;
    float3 coat = prefiltered * (0.04 * brdf.x + brdf.y);
    return base * (1.0 - cc * coat_fresnel(ndotv)) + cc * coat;
}

// Smooth radius-limited falloff (windowed inverse-square).
float point_attenuation(float dist, float radius) {
    float t = saturate(1.0 - pow(dist / radius, 4.0));
    return t * t / (dist * dist + 1.0);
}

// Fraction of the directional light reaching `world_pos` (1 = lit, 0 = shadowed),
// 3x3 PCF over the shadow map. Returns 1 when shadows are disabled or the point
// falls outside the map.
float shadow_factor(float3 world_pos, float ndotl) {
    if (pc.camera_pos.w < 0.5) {
        return 1.0;
    }
    float4 light_clip = mul(pc.light_view_proj, float4(world_pos, 1.0));
    float3 ndc = light_clip.xyz / light_clip.w;
    float2 uv = ndc.xy * 0.5 + 0.5;
    if (uv.x < 0.0 || uv.x > 1.0 || uv.y < 0.0 || uv.y > 1.0 || ndc.z > 1.0) {
        return 1.0;
    }

    uint width = 0;
    uint height = 0;
    g_shadow.GetDimensions(width, height);
    float2 texel = 1.0 / float2(width, height);
    float bias = max(0.0025 * (1.0 - ndotl), 0.0008);

    float sum = 0.0;
    [unroll]
    for (int dy = -1; dy <= 1; ++dy) {
        [unroll]
        for (int dx = -1; dx <= 1; ++dx) {
            float closest =
                g_shadow.SampleLevel(g_shadow_sampler, uv + float2(dx, dy) * texel, 0).r;
            sum += (ndc.z - bias) > closest ? 0.0 : 1.0;
        }
    }
    return sum / 9.0;
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
    if (mat.a < 0.5) { // background: sky (pc.direction.w: 0 off, 1 procedural, 2 LUT)
        float3 bg = float3(0.0, 0.0, 0.0);
        if (pc.direction.w > 0.5) {
            float bg_depth = g_depth.Load(int3(coord, 0)).r;
            float3 far_pos = world_from_depth(coord, width, height, bg_depth);
            float3 ray = normalize(far_pos - pc.camera_pos.xyz);
            float3 sun = normalize(pc.direction.xyz);
            if (pc.direction.w > 1.5) {
                // Physically-based sky: atmosphere from the sky-view LUT,
                // volumetric clouds marched per pixel (sharp shapes — the LUT
                // is too coarse for them), analytic sun disc tinted by the
                // transmittance and dimmed behind clouds.
                bg = sample_skyview(g_skyview, g_ibl_sampler, ray, sun).rgb;
                float3 cloud_ambient =
                    sample_skyview(g_skyview, g_ibl_sampler, float3(0.2, 0.97, 0.0), sun).rgb *
                        0.8 +
                    float3(0.008, 0.011, 0.016);
                float4 clouds =
                    march_clouds(ray, sun, g_transmittance_lut, g_ibl_sampler, cloud_ambient);
                bg = (bg * clouds.a + clouds.rgb) * SKY_LUT_BRIGHTNESS;
                float cos_sun = dot(ray, sun);
                if (cos_sun > 0.9998) {
                    float3 sun_trans = sample_transmittance(
                        g_transmittance_lut, g_ibl_sampler, ATMOS_VIEW_HEIGHT, sun.y);
                    bg += sun_trans * clouds.a * 80.0 * smoothstep(0.9998, 0.99995, cos_sun);
                }
            } else {
                bg = sky(ray, sun);
            }
        }
        g_hdr[coord] = float4(bg, 1.0);
        return;
    }

    float4 albedo = g_albedo.Load(int3(coord, 0));
    float2 enc    = g_normal.Load(int3(coord, 0)).xy;
    float  depth  = g_depth.Load(int3(coord, 0)).r;

    float metallic  = mat.r;
    float roughness = mat.g;
    float occlusion = mat.b;
    float2 coat     = g_clearcoat.Load(int3(coord, 0)).rg;
    float  cc       = coat.r;
    float  ccr      = max(coat.g, 0.03); // floor keeps the smooth-coat GGX finite

    float3 n = oct_decode(enc);
    float3 world_pos = world_from_depth(coord, width, height, depth);
    float3 v = normalize(pc.camera_pos.xyz - world_pos);

    // Directional light (shadowed). With the atmosphere active, the sun's
    // light is tinted by its transmittance through the air mass — low sun
    // reddens the direct lighting, matching the LUT sky.
    float3 ldir   = normalize(pc.direction.xyz);
    float3 sun_radiance = pc.color.rgb * pc.color.w;
    if (pc.direction.w > 1.5) {
        sun_radiance *= sample_transmittance(g_transmittance_lut, g_ibl_sampler,
                                             ATMOS_VIEW_HEIGHT, ldir.y);
    }
    float  shadow = shadow_factor(world_pos, max(dot(n, ldir), 0.0));
    float3 sun    = surface_brdf(n, v, ldir, albedo.rgb, metallic, roughness);
    float3 color  = layer_clearcoat_direct(sun, n, v, ldir, cc, ccr) * sun_radiance * shadow;

    // Point lights (no shadows).
    uint light_count = uint(pc.ambient.w);
    for (uint i = 0; i < light_count; ++i) {
        PointLight pl = g_point_lights[i];
        float3 to = pl.position_radius.xyz - world_pos;
        float dist = length(to);
        if (dist >= pl.position_radius.w) {
            continue;
        }
        float3 lp = to / max(dist, 1e-4);
        float att = point_attenuation(dist, pl.position_radius.w);
        float3 base = surface_brdf(n, v, lp, albedo.rgb, metallic, roughness);
        color += layer_clearcoat_direct(base, n, v, lp, cc, ccr) *
                 pl.color_intensity.rgb * pl.color_intensity.w * att;
    }

    // Ambient: image-based lighting when the environment is enabled (same flag as
    // the sky background, since the IBL reflects that sky), else a flat constant.
    if (pc.direction.w > 0.5) {
        float3 ambient = ibl_ambient(n, v, albedo.rgb, metallic, roughness, occlusion);
        color += layer_clearcoat_ibl(ambient, n, v, cc, ccr);
    } else {
        color += pc.ambient.rgb * albedo.rgb * occlusion;
    }
    g_hdr[coord] = float4(color, 1.0);
}
