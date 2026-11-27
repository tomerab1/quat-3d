// lighting.slang — deferred PBR lighting pass (Phase 3 Slice 3.6; PBR in 4.5).
//
// A fullscreen compute pass: reads the GBuffer (albedo, octahedral world normal,
// packed metallic/roughness/occlusion, depth) and shades one directional light
// with a Cook-Torrance microfacet BRDF (GGX NDF, height-correlated Smith
// visibility, Schlick Fresnel) plus a flat ambient term, writing linear HDR
// colour. World position is reconstructed from depth via the inverse view-proj,
// giving the per-pixel view vector specular needs. The material target's alpha is
// the coverage mask (1 where geometry was rasterised, 0 for background).
// Point/spot lights are clustered (R10.1): each pixel loops only over the light
// list of the froxel it falls in, built by the light_cull compute pass.
// R12: the sun is shadowed by the cascaded shadow map (lib/csm.slang) times a
// contact-shadow march; GTAO (gtao.slang) occludes the ambient term — bent
// normals for the diffuse irradiance lookup, multi-bounce, and cone-based
// specular occlusion (lib/occlusion.slang). Resource bindings come from
// reflection (no explicit indices); per-frame values are in g_frame.
// R21: energy-conserving specular (lib/brdf_energy.slang: Fdez-Aguera
// multiple scattering for the environment, Kulla-Conty compensation for
// punctual lights), horizon occlusion of normal-mapped reflections against
// the geometric normal reconstructed from depth, screen-space reflections
// (SsrPass; they replace the environment's specular radiance where they
// hit) and an HDRI sky (sky mode 3: the HdriEnvironment cube).

import atmosphere;
import clustered_lights;
import csm;
import occlusion;
import octahedral;
import sky;
import pbr; // pbr_punctual_alpha
import hdr; // hdr_store
import brdf_energy;

static const float PI = 3.14159265358979;

// Scales the sky-view LUT's luminance (per-unit sun irradiance) into the
// engine's HDR lighting range.
static const float SKY_LUT_BRIGHTNESS = 8.0;

// Prefiltered specular cubemap mip count - 1 (the highest roughness LOD). Must
// match prefilter_mip_count in ibl_pass.cpp.
static const float PREFILTER_MAX_LOD = 4.0;

// Per-frame values (std140, 304 bytes; LightingConstants in lighting_pass.cpp).
// Matrices are glm columns (applied with mul_cols).
struct LightingConstants {
    float4 inv_view_proj[4]; // clip -> world (jittered), reconstructs positions
    float4 view_proj[4];     // world -> clip (jittered); clip.w = view distance
    float4 camera_pos;       // xyz = world-space camera position, w = sun shadows (CSM) on
    float4 direction;        // xyz = direction TO the light (normalised), w = sky mode
    float4 color;            // rgb = colour, w = intensity
    float4 ambient;          // rgb = flat ambient colour, w = IBL intensity
    float4 cloud_a;          // coverage, extinction /km, bottom km, thickness km
    float4 cloud_b;          // 1/size km, detail erosion, drift x km, drift z km
    float4 csm_filter;       // PCF radius, tan(sun size), normal bias, depth bias
    float4 csm_misc;         // x debug cascades, y noise offset (frame), zw depth -> view (A, B)
    float4 contact;          // x intensity (0 = off), y length m, z steps, w thickness m
    float4 ao;               // x GTAO on, y bent normals, z specular occlusion, w multi-bounce
    float4 realism;          // x multiscatter, y horizon occlusion, z reflections bound (R21)
}
ConstantBuffer<LightingConstants> g_frame;

Texture2D                               g_albedo;
Texture2D                               g_normal;
Texture2D                               g_material;
Texture2D                               g_depth;
[format("rgba16f")] RWTexture2D<float4> g_hdr;
Texture2D                               g_clearcoat; // r = coat strength, g = coat roughness

// Cascaded sun shadows (bound to fallbacks when camera_pos.w < 0.5).
Texture2DArray                g_shadow_map;
SamplerComparisonState        g_shadow_cmp;
SamplerState                  g_point_sampler;
StructuredBuffer<CascadeData> g_cascades;

// GTAO: rgb = world bent normal * 0.5 + 0.5, a = visibility (used when ao.x > 0.5).
Texture2D                     g_ao;

// Split-sum IBL maps (precomputed once in ibl_pass.cpp): diffuse irradiance cube,
// prefiltered specular cube (roughness -> mip), and the BRDF integration LUT.
TextureCube  g_irradiance;
TextureCube  g_prefiltered;
Texture2D    g_brdf_lut;
SamplerState g_ibl_sampler;
// Atmosphere LUTs (Phase 11.1; bound to fallbacks when direction.w < 2).
Texture2D    g_skyview;
Texture2D    g_transmittance_lut;
// Clustered lights (light_cull.slang output; dims.w = 0 when there are none).
ConstantBuffer<ClusterParams> g_cluster_params;
StructuredBuffer<GpuLight>    g_lights;
StructuredBuffer<uint2>       g_cluster_grid;    // offset, count
StructuredBuffer<uint>        g_cluster_indices;
// R21: screen-space reflections (SsrPass; rgb radiance, a weight; read when
// realism.z > 0.5) and the HDRI environment cube (sky mode 3 background).
Texture2D                     g_reflections;
TextureCube                   g_environment;

// Schlick Fresnel with a roughness term, for the ambient/environment specular
// (the view-independent IBL term, unlike the per-light f_schlick).
float3 fresnel_schlick_roughness(float ndotv, float3 f0, float roughness) {
    float3 fmax = max(float3(1.0 - roughness, 1.0 - roughness, 1.0 - roughness), f0);
    return f0 + (fmax - f0) * pow(saturate(1.0 - ndotv), 5.0);
}

// Image-based ambient: diffuse irradiance + prefiltered specular reflection,
// combined through the split-sum BRDF LUT (`dfg` at NdotV, roughness).
// Replaces the flat constant ambient. `diffuse_n` is the irradiance lookup
// direction (the GTAO bent normal when available); the occlusion terms apply
// per lobe. R21: the multiple-scattering lobe and the energy left to diffuse
// follow lib/brdf_energy.slang, and `reflection` (screen-space hits: rgb
// radiance, a weight) replaces the occluded environment radiance.
float3 ibl_ambient(float3 N, float3 diffuse_n, float3 V, float3 albedo, float metallic,
                   float roughness, float3 diffuse_occlusion, float specular_occlusion,
                   float2 dfg, float4 reflection) {
    float ndotv = max(dot(N, V), 1e-4);
    float3 f0 = lerp(float3(0.04, 0.04, 0.04), albedo, metallic);
    float3 F = fresnel_schlick_roughness(ndotv, f0, roughness);
    IblEnergy e = ibl_energy(f0, F, dfg, albedo * (1.0 - metallic), g_frame.realism.x > 0.5);

    float3 irradiance = g_irradiance.SampleLevel(g_ibl_sampler, diffuse_n, 0.0).rgb;
    float3 R = reflect(-V, N);
    float3 prefiltered =
        g_prefiltered.SampleLevel(g_ibl_sampler, R, roughness * PREFILTER_MAX_LOD).rgb;
    float3 radiance = lerp(prefiltered * specular_occlusion, reflection.rgb, reflection.a);
    return e.single * radiance + (e.multi + e.diffuse) * irradiance * diffuse_occlusion;
}

// Reconstruct world position from the pixel's screen UV + sampled depth. The
// inverse of the same view_proj used to rasterise (which already carries the
// Vulkan clip-space Y flip), so no manual flip here.
float3 world_from_depth(int2 coord, uint width, uint height, float depth) {
    float2 uv  = (float2(coord) + 0.5) / float2(width, height);
    float2 ndc = uv * 2.0 - 1.0;
    float4 clip = float4(ndc, depth, 1.0);
    float4 world = mul_cols(g_frame.inv_view_proj, clip);
    return world.xyz / world.w;
}

// R21: the geometric (unperturbed) normal from the depth buffer, for horizon
// occlusion. Each axis takes the neighbour whose depth is closer to this
// pixel's (no smearing across silhouettes; the screen edge uses the inner
// side), and the result faces the viewer. Falls back to `shading_n`.
float3 geometric_normal(int2 coord, uint width, uint height, float depth, float3 p, float3 v,
                        float3 shading_n) {
    int2 last = int2(int(width), int(height)) - 1;
    int2 cl = int2(max(coord.x - 1, 0), coord.y);
    int2 cr = int2(min(coord.x + 1, last.x), coord.y);
    int2 cu = int2(coord.x, max(coord.y - 1, 0));
    int2 cd = int2(coord.x, min(coord.y + 1, last.y));
    float dl = g_depth.Load(int3(cl, 0)).r;
    float dr = g_depth.Load(int3(cr, 0)).r;
    float du = g_depth.Load(int3(cu, 0)).r;
    float dd = g_depth.Load(int3(cd, 0)).r;
    bool right = coord.x == 0 || (coord.x < last.x && abs(dr - depth) < abs(dl - depth));
    bool down = coord.y == 0 || (coord.y < last.y && abs(dd - depth) < abs(du - depth));
    float3 px = right ? world_from_depth(cr, width, height, dr) - p
                      : p - world_from_depth(cl, width, height, dl);
    float3 py = down ? world_from_depth(cd, width, height, dd) - p
                     : p - world_from_depth(cu, width, height, du);
    float3 ng = cross(px, py);
    float len = length(ng);
    if (!(len > 1e-12)) {
        return shading_n;
    }
    ng /= len;
    return dot(ng, v) < 0.0 ? -ng : ng;
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
// radiance — returns (diffuse + specular) * NdotL. `energy` scales the
// specular lobe (R21 multiple-scattering compensation; 1 = single scatter).
float3 surface_brdf(float3 N, float3 V, float3 L, float3 albedo, float metallic, float roughness,
                    float3 energy) {
    float3 h = normalize(V + L);
    float ndotl = max(dot(N, L), 0.0);
    float ndotv = max(dot(N, V), 1e-4);
    float ndoth = max(dot(N, h), 0.0);
    float vdoth = max(dot(V, h), 0.0);

    float  a  = pbr_punctual_alpha(roughness); // finite highlight (issue #45)
    float3 f0 = lerp(float3(0.04), albedo, metallic);

    float  D   = d_ggx(ndoth, a);
    float  Vis = v_smith(ndotv, ndotl, a);
    float3 F   = f_schlick(vdoth, f0);

    float3 spec    = D * Vis * F * energy;
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
    float ac = pbr_punctual_alpha(ccr);
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

// The sun's visibility at `world_pos`: cascaded shadow map times the
// contact-shadow march. `cascade` receives the cascade used (debug view).
float sun_visibility(int2 coord, uint2 size, float3 world_pos, float3 n, float3 ldir,
                     out uint cascade) {
    cascade = CSM_MAX_CASCADES;
    float noise = interleaved_gradient_noise(float2(coord) + g_frame.csm_misc.y);
    float4 clip = mul_cols(g_frame.view_proj, float4(world_pos, 1.0));
    float vis = 1.0;
    if (g_frame.camera_pos.w > 0.5) {
        CsmFilter f;
        f.pcf_radius = g_frame.csm_filter.x;
        f.light_tan = g_frame.csm_filter.y;
        f.normal_bias = g_frame.csm_filter.z;
        f.depth_bias = g_frame.csm_filter.w;
        vis = csm_visibility(g_cascades[0], g_shadow_map, g_shadow_cmp, g_point_sampler,
                             world_pos, n, ldir, clip.w, f, noise, cascade);
    }
    if (g_frame.contact.x > 0.0 && vis > 0.0 && dot(n, ldir) > 0.0) {
        // Start just off the surface so the march does not re-hit its own pixel.
        float3 start = world_pos + n * (clip.w * 0.002);
        float4 clip0 = mul_cols(g_frame.view_proj, float4(start, 1.0));
        float4 clip1 =
            mul_cols(g_frame.view_proj, float4(start + ldir * g_frame.contact.y, 1.0));
        float hit = contact_shadow(g_depth, g_point_sampler, size, clip0, clip1,
                                   g_frame.csm_misc.zw, uint(g_frame.contact.z),
                                   g_frame.contact.w, noise);
        vis *= lerp(1.0, hit, g_frame.contact.x);
    }
    return vis;
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
    if (mat.a < 0.5) { // background: sky (g_frame.direction.w: 0 off, 1 procedural, 2 LUT)
        float3 bg = float3(0.0, 0.0, 0.0);
        if (g_frame.direction.w > 0.5) {
            float bg_depth = g_depth.Load(int3(coord, 0)).r;
            float3 far_pos = world_from_depth(coord, width, height, bg_depth);
            float3 ray = normalize(far_pos - g_frame.camera_pos.xyz);
            float3 sun = normalize(g_frame.direction.xyz);
            if (g_frame.direction.w > 2.5) {
                // R21: HDRI environment (rotation and intensity baked in).
                bg = g_environment.SampleLevel(g_ibl_sampler, ray, 0.0).rgb;
            } else if (g_frame.direction.w > 1.5) {
                // Physically-based sky: atmosphere from the sky-view LUT,
                // volumetric clouds marched per pixel (sharp shapes — the LUT
                // is too coarse for them), analytic sun disc tinted by the
                // transmittance and dimmed behind clouds.
                bg = sample_skyview(g_skyview, g_ibl_sampler, ray, sun).rgb;
                float3 cloud_ambient =
                    sample_skyview(g_skyview, g_ibl_sampler, float3(0.2, 0.97, 0.0), sun).rgb *
                        0.8 +
                    float3(0.008, 0.011, 0.016);
                CloudParams cp = unpack_cloud_params(g_frame.cloud_a, g_frame.cloud_b);
                float jitter = interleaved_gradient_noise(float2(coord));
                float4 clouds = march_clouds(ray, sun, g_transmittance_lut, g_ibl_sampler,
                                             cloud_ambient, cp, jitter);
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
        g_hdr[coord] = float4(hdr_store(bg), 1.0);
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
    float3 v = normalize(g_frame.camera_pos.xyz - world_pos);

    // Directional light (shadowed). With the atmosphere active, the sun's
    // light is tinted by its transmittance through the air mass — low sun
    // reddens the direct lighting, matching the LUT sky.
    float3 ldir   = normalize(g_frame.direction.xyz);
    float3 sun_radiance = g_frame.color.rgb * g_frame.color.w;
    if (g_frame.direction.w > 1.5 && g_frame.direction.w < 2.5) {
        sun_radiance *= sample_transmittance(g_transmittance_lut, g_ibl_sampler,
                                             ATMOS_VIEW_HEIGHT, ldir.y);
    }
    // R21: the split-sum DFG at this pixel drives the multiple-scattering
    // energy of both the punctual lights and the environment (only with real
    // IBL maps bound: the fallback LUT is black, so realism.x is 0 then).
    float  ndotv  = max(dot(n, v), 1e-4);
    float2 dfg    = g_brdf_lut.SampleLevel(g_ibl_sampler, float2(ndotv, roughness), 0.0).rg;
    float3 energy = punctual_energy_compensation(lerp(float3(0.04), albedo.rgb, metallic), dfg,
                                                 g_frame.realism.x > 0.5);
    uint   cascade = CSM_MAX_CASCADES;
    float  shadow = sun_visibility(coord, uint2(width, height), world_pos, n, ldir, cascade);
    float3 sun    = surface_brdf(n, v, ldir, albedo.rgb, metallic, roughness, energy);
    float3 color  = layer_clearcoat_direct(sun, n, v, ldir, cc, ccr) * sun_radiance * shadow;

    // Clustered point/spot lights (no shadows): only this pixel's froxel list.
    if (g_cluster_params.dims.w > 0) {
        uint2 range = g_cluster_grid[cluster_index_of_world(g_cluster_params, world_pos)];
        for (uint k = 0; k < range.y; ++k) {
            GpuLight light = g_lights[g_cluster_indices[range.x + k]];
            float3 lp;
            float3 radiance;
            if (!light_incident(light, world_pos, lp, radiance)) {
                continue;
            }
            float3 base = surface_brdf(n, v, lp, albedo.rgb, metallic, roughness, energy);
            color += layer_clearcoat_direct(base, n, v, lp, cc, ccr) * radiance;
        }
    }

    // Ambient: image-based lighting when the environment is enabled (same flag as
    // the sky background, since the IBL reflects that sky), else a flat constant.
    // GTAO combines with the material's baked occlusion (the darker wins);
    // without it the material occlusion applies to both lobes as before.
    float3 diffuse_n = n;
    float3 diffuse_occ = float3(occlusion, occlusion, occlusion);
    float  specular_occ = occlusion;
    if (g_frame.ao.x > 0.5) {
        float4 ao = g_ao.Load(int3(coord, 0));
        float visibility = min(ao.a, occlusion);
        float3 bent = g_frame.ao.y > 0.5 ? normalize(ao.rgb * 2.0 - 1.0) : n;
        diffuse_n = bent;
        diffuse_occ = g_frame.ao.w > 0.5 ? gtao_multi_bounce(visibility, albedo.rgb)
                                         : float3(visibility, visibility, visibility);
        specular_occ = visibility;
        if (g_frame.ao.z > 0.5) {
            float3 r = reflect(-v, n);
            specular_occ = min(occlusion, gtao_specular_occlusion(bent, ao.a, r, roughness));
        }
    }
    if (g_frame.direction.w > 0.5) {
        if (g_frame.realism.y > 0.5) {
            float3 ng = geometric_normal(coord, width, height, depth, world_pos, v, n);
            specular_occ *= horizon_occlusion(reflect(-v, n), ng);
        }
        float4 reflection = g_frame.realism.z > 0.5 ? g_reflections.Load(int3(coord, 0))
                                                    : float4(0.0, 0.0, 0.0, 0.0);
        float3 ambient = ibl_ambient(n, diffuse_n, v, albedo.rgb, metallic, roughness,
                                     diffuse_occ, specular_occ, dfg, reflection);
        color += layer_clearcoat_ibl(ambient, n, v, cc, ccr) * g_frame.ambient.w;
    } else {
        color += g_frame.ambient.rgb * albedo.rgb * diffuse_occ;
    }
    if (g_frame.csm_misc.x > 0.5) {
        color *= csm_debug_color(cascade);
    }
    g_hdr[coord] = float4(hdr_store(color), 1.0);
}
