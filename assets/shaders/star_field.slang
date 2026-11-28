// star_field.slang — bright stars as physical point sources (R22.2.1).
//
// Each catalogue star (J2000 direction, illuminance above the atmosphere,
// Y-normalised colour from its B−V) is
//   1. rotated to the world by SkyState's celestial matrix (precession,
//      sidereal time, latitude) and projected with the frame's jittered
//      matrix (the star moves with the TAA jitter, so the resolve converges
//      on a stable point);
//   2. attenuated by the atmosphere: the transmittance LUT along its line of
//      sight (airmass extinction, reddened near the horizon) and the cloud
//      layer's transmittance (the same march the sky uses);
//   3. made to twinkle: log-normal intensity scintillation whose strength
//      grows with airmass as X^1.75 (Young, "Scintillation of stars",
//      AJ 72 (1967) 747; Dravins et al., "Atmospheric intensity scintillation
//      of stars", PASP 109 (1997) 173), with independent colour channels near
//      the horizon (chromatic scintillation), and mean 1 so it conserves
//      energy over time;
//   4. splatted as an energy-conserving sub-pixel Gaussian PSF: the weights
//      over a 5x5 footprint are normalised to 1, so the star's summed pixel
//      luminance x pixel solid angle equals its illuminance wherever its
//      centre falls inside a pixel (no shimmer as it drifts). Radiance is
//      L = E w / Omega_pixel, in pre-exposed HDR units (photometry.hpp).
// Daylight hides stars by exposure alone: their luminance sits ~4 decades
// below a noon sky.
//
// splat_main (one thread per star) bins visible stars into 16x16-pixel
// screen tiles; resolve_main (one thread per pixel) sums the stars of its
// tile into the HDR over the sky (depth == 1) only. Every pixel writes only
// itself: no atomics on colour, no races.

import atmosphere;
import hdr; // hdr_store

// Mirrors StarFrameGpu in star_field_pass.cpp (std140, 224 bytes).
struct StarFrame {
    float4 view_proj[4];            // world -> clip (jittered), glm columns
    float4 world_from_celestial[3]; // J2000 -> world, columns (xyz)
    float4 forward;                 // xyz camera forward, w pixel solid angle on axis (sr)
    float4 extent;                  // xy render extent, zw tile counts
    float4 params;                  // x lux -> pre-exposed units, y time s,
                                    // z scintillation strength, w star count
    float4 psf;                     // x sigma px, y 1 / (2 sigma^2), z tile capacity,
                                    // w horizon cosine
    float4 sun;                     // xyz toward the sun, w clouds on
    float4 cloud_a;
    float4 cloud_b;
}

// One catalogue star (star_field_pass.cpp StarGpu).
struct Star {
    float4 direction_lux; // xyz J2000 unit vector, w illuminance (lux)
    float4 color_seed;    // rgb (luminance 1), w hash seed in [0, 1)
}

// A star prepared for the resolve.
struct VisibleStar {
    float4 position; // xy pixel centre (pixel-index space), z 1 / sum of weights
    float4 radiance; // rgb pre-exposed E / Omega_pixel
}

[[vk::binding(0)]] ConstantBuffer<StarFrame>       g_frame;
[[vk::binding(1)]] StructuredBuffer<Star>          g_stars;
[[vk::binding(2)]] RWStructuredBuffer<VisibleStar> g_visible;
[[vk::binding(3)]] RWStructuredBuffer<uint>        g_tile_counts;
[[vk::binding(4)]] RWStructuredBuffer<uint>        g_tile_lists;
[[vk::binding(5)]] Texture2D                       g_transmittance;
[[vk::binding(6)]] SamplerState                    g_samp;
[[vk::binding(7)]] Texture2D                       g_depth;
[[vk::binding(8)]] [format("rgba16f")] RWTexture2D<float4> g_hdr;

static const int  FOOTPRINT = 2; // pixels each side of the centre pixel
static const uint TILE = 16;

float4 mul_cols4(float4 cols[4], float4 v) {
    return cols[0] * v.x + cols[1] * v.y + cols[2] * v.z + cols[3] * v.w;
}

// Zero-mean, unit-variance smooth noise in time: three sinusoids with
// per-star frequencies (2-14 Hz, the band of naked-eye twinkling) and phases.
float twinkle_noise(float t, float seed) {
    float n = 0.0;
    [unroll]
    for (int k = 0; k < 3; ++k) {
        float h1 = frac(seed * (17.13 + 5.7 * float(k)) + 0.31 * float(k));
        float h2 = frac(seed * (91.71 + 13.3 * float(k)) + 0.77 * float(k));
        float freq = 2.0 + 12.0 * h1;
        n += sin(6.2831853 * (freq * t + h2));
    }
    return n * 0.81649658; // sqrt(2/3): each sinusoid has variance 1/2
}

// Integer pixel at the centre of a star's footprint.
int2 centre_pixel(float2 c) { return int2(floor(c + 0.5)); }

float psf_weight(float2 d) { return exp(-dot(d, d) * g_frame.psf.y); }

[shader("compute")]
[numthreads(64, 1, 1)]
void splat_main(uint3 tid : SV_DispatchThreadID) {
    uint i = tid.x;
    if (i >= uint(g_frame.params.w)) return;
    Star s = g_stars[i];
    float3 dir = normalize(g_frame.world_from_celestial[0].xyz * s.direction_lux.x +
                           g_frame.world_from_celestial[1].xyz * s.direction_lux.y +
                           g_frame.world_from_celestial[2].xyz * s.direction_lux.z);
    // Below the horizon the planet (the sky-view ground) hides it.
    if (dir.y < g_frame.psf.w) return;
    float4 clip = mul_cols4(g_frame.view_proj, float4(dir, 0.0));
    if (clip.w <= 1e-6) return;
    float2 ndc = clip.xy / clip.w;
    float2 c = (ndc * 0.5 + 0.5) * g_frame.extent.xy - 0.5;
    int2 centre = centre_pixel(c);
    int2 lo = centre - FOOTPRINT;
    int2 hi = centre + FOOTPRINT;
    if (hi.x < 0 || hi.y < 0 || lo.x >= int(g_frame.extent.x) || lo.y >= int(g_frame.extent.y)) {
        return;
    }

    // Atmosphere: extinction along the line of sight, then the clouds.
    float3 trans = sample_transmittance(g_transmittance, g_samp, ATMOS_VIEW_HEIGHT, dir.y);
    float clouds = 1.0;
    if (g_frame.sun.w > 0.5) {
        CloudParams cp = unpack_cloud_params(g_frame.cloud_a, g_frame.cloud_b);
        clouds = march_clouds(dir, normalize(g_frame.sun.xyz), g_transmittance, g_samp,
                              float3(0.0, 0.0, 0.0), cp, 0.5).a;
    }
    // Scintillation: log-normal, sigma growing as airmass^1.75.
    float airmass = 1.0 / max(dir.y, 0.03);
    float sigma = min(g_frame.params.z * 0.1 * pow(airmass, 1.75), 1.0);
    float t = g_frame.params.y;
    float seed = s.color_seed.w;
    float common_n = twinkle_noise(t, seed);
    float chroma = saturate((airmass - 2.0) / 10.0);
    float3 n = float3(lerp(common_n, twinkle_noise(t, frac(seed + 0.137)), chroma), common_n,
                      lerp(common_n, twinkle_noise(t, frac(seed + 0.611)), chroma));
    float3 scint = exp(sigma * n - 0.5 * sigma * sigma);

    // Pixel solid angle shrinks off axis as cos^3 of the angle to the view axis.
    float cos_axis = max(dot(dir, g_frame.forward.xyz), 1e-3);
    float omega = g_frame.forward.w * cos_axis * cos_axis * cos_axis;
    float3 radiance = s.color_seed.rgb * s.direction_lux.w * trans * clouds * scint *
                      g_frame.params.x / omega;
    if (!(max(radiance.r, max(radiance.g, radiance.b)) > 0.0)) return;

    float weight_sum = 0.0;
    for (int y = -FOOTPRINT; y <= FOOTPRINT; ++y) {
        for (int x = -FOOTPRINT; x <= FOOTPRINT; ++x) {
            weight_sum += psf_weight(float2(centre + int2(x, y)) - c);
        }
    }
    VisibleStar v;
    v.position = float4(c, 1.0 / max(weight_sum, 1e-6), 0.0);
    v.radiance = float4(radiance, 0.0);
    g_visible[i] = v;

    // Bin into every tile the footprint touches (at most 2 x 2).
    uint2 tiles = uint2(g_frame.extent.zw);
    int2 t_lo = max(lo, int2(0, 0)) / int(TILE);
    int2 t_hi = min(hi, int2(g_frame.extent.xy) - 1) / int(TILE);
    uint capacity = uint(g_frame.psf.z);
    for (int ty = t_lo.y; ty <= t_hi.y; ++ty) {
        for (int tx = t_lo.x; tx <= t_hi.x; ++tx) {
            uint tile = uint(ty) * tiles.x + uint(tx);
            uint slot;
            InterlockedAdd(g_tile_counts[tile], 1u, slot);
            if (slot < capacity) g_tile_lists[tile * capacity + slot] = i;
        }
    }
}

[shader("compute")]
[numthreads(8, 8, 1)]
void resolve_main(uint3 tid : SV_DispatchThreadID) {
    int2 p = int2(tid.xy);
    if (p.x >= int(g_frame.extent.x) || p.y >= int(g_frame.extent.y)) return;
    uint tile = (tid.y / TILE) * uint(g_frame.extent.z) + tid.x / TILE;
    uint capacity = uint(g_frame.psf.z);
    uint count = min(g_tile_counts[tile], capacity);
    if (count == 0u) return;
    if (g_depth.Load(int3(p, 0)).r < 1.0) return; // geometry covers the sky here

    float3 sum = float3(0.0, 0.0, 0.0);
    for (uint k = 0; k < count; ++k) {
        VisibleStar v = g_visible[g_tile_lists[tile * capacity + k]];
        int2 d = p - centre_pixel(v.position.xy);
        if (abs(d.x) > FOOTPRINT || abs(d.y) > FOOTPRINT) continue;
        sum += v.radiance.rgb * psf_weight(float2(p) - v.position.xy) * v.position.z;
    }
    if (max(sum.r, max(sum.g, sum.b)) <= 0.0) return;
    float4 c = g_hdr[p];
    g_hdr[p] = float4(hdr_store(c.rgb + sum), c.a);
}
