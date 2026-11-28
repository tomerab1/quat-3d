// cloud_noise.slang — tileable noise for the volumetric clouds (ROADMAP R13.4).
//
// The noise volumes of Schneider's Nubis ("The Real-time Volumetric
// Cloudscapes of Horizon: Zero Dawn", SIGGRAPH 2015 Advances; GPU Pro 7,
// "Real-Time Volumetric Cloudscapes", 2016): a 128³ base volume whose red
// channel is Perlin-Worley noise (gradient-noise fBm dilated by Worley fBm,
// billowy but connected) and whose green / blue / alpha are Worley fBm at
// rising frequencies, plus a 32³ detail volume of higher-frequency Worley
// fBm that erodes the edges, and a 2D curl-noise texture that distorts the
// detail lookups into wisps. All of it is baked once by compute
// (cloud_noise_bake.slang) and tiles in every axis: every lattice is
// wrapped by its period, so the textures repeat seamlessly with REPEAT
// addressing.
//
// Hashing is integer-only — pcg3d from Jarzynski & Olano, "Hash Functions
// for GPU Rendering" (JCGT 2020) — so the CPU mirror
// (engine::renderer::cloud_noise, cloud_noise.cpp) produces the same lattice
// bit for bit; only the float interpolation may differ by rounding.

public uint3 pcg3d(uint3 v) {
    v = v * 1664525u + 1013904223u;
    v.x += v.y * v.z;
    v.y += v.z * v.x;
    v.z += v.x * v.y;
    v ^= v >> 16u;
    v.x += v.y * v.z;
    v.y += v.z * v.x;
    v.z += v.x * v.y;
    return v;
}

// Three uniform floats in [0, 1) for lattice point `c` (wrapped by the caller).
public float3 hash33(uint3 c, uint seed) {
    uint3 h = pcg3d(c + uint3(seed, seed * 7u, seed * 13u));
    return float3(h >> 8u) * (1.0 / 16777216.0);
}

uint3 wrap3(int3 c, int period) {
    return uint3(((c % period) + period) % period);
}

// Quintic fade (Perlin, "Improving Noise", SIGGRAPH 2002).
float3 fade3(float3 t) {
    return t * t * t * (t * (t * 6.0 - 15.0) + 10.0);
}

// The gradient of lattice corner c: one of the 12 cube-edge directions
// (Perlin 2002), selected by the corner's hash.
float corner_dot(uint3 c, uint seed, float3 d) {
    uint h = pcg3d(c + uint3(seed, seed * 7u, seed * 13u)).x % 12u;
    float u = h < 8u ? d.x : d.y;
    float v = h < 4u ? d.y : d.z;
    return ((h & 1u) == 0u ? u : -u) + ((h & 2u) == 0u ? v : -v);
}

// Gradient noise at `p` (lattice units) with the lattice wrapped every
// `period` cells; roughly in [-1, 1].
public float gradient_noise(float3 p, int period, uint seed) {
    float3 fl = floor(p);
    int3 i = int3(fl);
    float3 f = p - fl;
    float3 u = fade3(f);
    float n000 = corner_dot(wrap3(i + int3(0, 0, 0), period), seed, f - float3(0, 0, 0));
    float n100 = corner_dot(wrap3(i + int3(1, 0, 0), period), seed, f - float3(1, 0, 0));
    float n010 = corner_dot(wrap3(i + int3(0, 1, 0), period), seed, f - float3(0, 1, 0));
    float n110 = corner_dot(wrap3(i + int3(1, 1, 0), period), seed, f - float3(1, 1, 0));
    float n001 = corner_dot(wrap3(i + int3(0, 0, 1), period), seed, f - float3(0, 0, 1));
    float n101 = corner_dot(wrap3(i + int3(1, 0, 1), period), seed, f - float3(1, 0, 1));
    float n011 = corner_dot(wrap3(i + int3(0, 1, 1), period), seed, f - float3(0, 1, 1));
    float n111 = corner_dot(wrap3(i + int3(1, 1, 1), period), seed, f - float3(1, 1, 1));
    float x00 = lerp(n000, n100, u.x);
    float x10 = lerp(n010, n110, u.x);
    float x01 = lerp(n001, n101, u.x);
    float x11 = lerp(n011, n111, u.x);
    return lerp(lerp(x00, x10, u.y), lerp(x01, x11, u.y), u.z);
}

// Worley (cellular) noise, Worley 1996 "A Cellular Texture Basis Function":
// 1 - F1, the distance to the nearest feature point (one per cell, cells
// wrapped every `period`), clamped to [0, 1]. Bright cell centres, dark
// borders: the billows of cumulus.
public float worley_noise(float3 p, int period, uint seed) {
    float3 fl = floor(p);
    int3 i = int3(fl);
    float3 f = p - fl;
    float best = 1.0e9;
    for (int z = -1; z <= 1; ++z) {
        for (int y = -1; y <= 1; ++y) {
            for (int x = -1; x <= 1; ++x) {
                int3 o = int3(x, y, z);
                float3 feature = float3(o) + hash33(wrap3(i + o, period), seed);
                float3 d = feature - f;
                best = min(best, dot(d, d));
            }
        }
    }
    return 1.0 - saturate(sqrt(best));
}

// Linear remap of v from [lo, hi] to [new_lo, new_hi] (Schneider 2015).
public float remap(float v, float lo, float hi, float new_lo, float new_hi) {
    return new_lo + (v - lo) * (new_hi - new_lo) / (hi - lo);
}

// Three octaves of Worley noise starting at `freq` cells per tile (weights
// 0.625 / 0.25 / 0.125, GPU Pro 7).
public float worley_fbm(float3 uvw, int freq, uint seed) {
    return worley_noise(uvw * float(freq), freq, seed) * 0.625 +
           worley_noise(uvw * float(freq * 2), freq * 2, seed) * 0.25 +
           worley_noise(uvw * float(freq * 4), freq * 4, seed) * 0.125;
}

// Gradient-noise fBm in [0, 1], `octaves` octaves from `freq` (gain 0.5).
public float perlin_fbm(float3 uvw, int freq, int octaves, uint seed) {
    float sum = 0.0;
    float amp = 0.5;
    float norm = 0.0;
    int f = freq;
    for (int o = 0; o < octaves; ++o) {
        sum += amp * gradient_noise(uvw * float(f), f, seed + uint(o));
        norm += amp;
        amp *= 0.5;
        f *= 2;
    }
    return saturate(sum / norm * 0.5 + 0.5);
}

// Seeds of the baked volumes (the CPU mirror uses the same).
public static const uint CLOUD_SEED_PERLIN = 3u;
public static const uint CLOUD_SEED_WORLEY = 11u;
public static const uint CLOUD_SEED_DETAIL = 29u;
public static const uint CLOUD_SEED_CURL = 41u;
public static const uint CLOUD_SEED_WEATHER = 53u;

// Measured range of the weather's Perlin-Worley field and of the base
// volume's shape (remap(r, low_fbm - 1, 1, 0, 1)): their 0.5th / 99.5th
// percentiles, so both can be stretched to [0, 1] and coverage means the
// covered fraction (tests/renderer/cloud_noise_test.cpp checks them).
public static const float CLOUD_WEATHER_LO = 0.55;
public static const float CLOUD_WEATHER_HI = 0.92;
public static const float CLOUD_SHAPE_LO = 0.70;
public static const float CLOUD_SHAPE_HI = 0.93;

// The base volume at uvw in [0, 1)³ (one tile): r = Perlin-Worley,
// g / b / a = Worley fBm at 8 / 16 / 32 cells per tile.
public float4 cloud_base_noise(float3 uvw) {
    float perlin = perlin_fbm(uvw, 4, 5, CLOUD_SEED_PERLIN);
    float worley = worley_fbm(uvw, 4, CLOUD_SEED_WORLEY);
    // Dilate the gradient noise by the Worley fBm: Perlin's connectedness
    // with Worley's billows (Schneider 2015).
    float perlin_worley = saturate(remap(perlin, 0.0, 1.0, worley, 1.0));
    return float4(perlin_worley, worley_fbm(uvw, 8, CLOUD_SEED_WORLEY + 1u),
                  worley_fbm(uvw, 16, CLOUD_SEED_WORLEY + 2u),
                  worley_fbm(uvw, 32, CLOUD_SEED_WORLEY + 3u));
}

// The detail volume at uvw: Worley fBm at 2 / 4 / 8 cells per tile.
public float4 cloud_detail_noise(float3 uvw) {
    return float4(worley_fbm(uvw, 2, CLOUD_SEED_DETAIL),
                  worley_fbm(uvw, 4, CLOUD_SEED_DETAIL + 1u),
                  worley_fbm(uvw, 8, CLOUD_SEED_DETAIL + 2u), 1.0);
}

// Curl of a vector potential made of three gradient-noise fields on the
// z = 0 slice (Bridson, Hourihan & Nordenstam, "Curl-Noise for Procedural
// Fluid Flow", SIGGRAPH 2007), by central differences; the xy components,
// mapped to [0, 1] (0.5 = no distortion). Divergence-free, so the detail
// lookups swirl instead of smearing.
public float2 cloud_curl_noise(float2 uv) {
    const int freq = 4;
    const float h = 1.0 / 512.0;
    float3 p = float3(uv, 0.0) * float(freq);
    float e = h * float(freq);
    // psi = (psi1, psi2, psi3); curl.xy = (d psi3/dy - d psi2/dz,
    // d psi1/dz - d psi3/dx).
    float d3dy = gradient_noise(p + float3(0, e, 0), freq, CLOUD_SEED_CURL + 2u) -
                 gradient_noise(p - float3(0, e, 0), freq, CLOUD_SEED_CURL + 2u);
    float d2dz = gradient_noise(p + float3(0, 0, e), freq, CLOUD_SEED_CURL + 1u) -
                 gradient_noise(p - float3(0, 0, e), freq, CLOUD_SEED_CURL + 1u);
    float d1dz = gradient_noise(p + float3(0, 0, e), freq, CLOUD_SEED_CURL) -
                 gradient_noise(p - float3(0, 0, e), freq, CLOUD_SEED_CURL);
    float d3dx = gradient_noise(p + float3(e, 0, 0), freq, CLOUD_SEED_CURL + 2u) -
                 gradient_noise(p - float3(e, 0, 0), freq, CLOUD_SEED_CURL + 2u);
    float2 curl = float2(d3dy - d2dz, d1dz - d3dx) / (2.0 * e);
    return saturate(curl * 0.25 + 0.5);
}

// The weather noise (one tile): r = coverage field (Perlin-Worley fBm, mean
// ~0.5), g = cloud-type variation, b = precipitation variation. The cloud
// shader turns it into local coverage / type from the frame's weather
// (lib/clouds.slang cloud_weather), so a weather change needs no re-bake.
public float4 cloud_weather_noise(float2 uv) {
    float3 p = float3(uv, 0.5);
    float perlin = perlin_fbm(p, 4, 5, CLOUD_SEED_WEATHER);
    float worley = worley_fbm(p, 4, CLOUD_SEED_WEATHER + 1u);
    // Perlin-Worley spans ~[0.55, 0.92] (cloud_noise_test measures it);
    // stretched to [0, 1] so the field's mean is ~0.5.
    float coverage = saturate(remap(saturate(remap(perlin, 0.0, 1.0, worley, 1.0)),
                                    CLOUD_WEATHER_LO, CLOUD_WEATHER_HI, 0.0, 1.0));
    return float4(coverage, perlin_fbm(p, 2, 3, CLOUD_SEED_WEATHER + 2u),
                  perlin_fbm(p, 3, 3, CLOUD_SEED_WEATHER + 3u), 1.0);
}
