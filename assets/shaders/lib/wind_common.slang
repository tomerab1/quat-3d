// wind_common.slang — the global wind field on the GPU (R13.1).
//
// Operation-for-operation mirror of the CPU reference in
// src/weather/wind_field.cpp (engine::weather::sample_wind): the mean wind
// modulated by gust fronts (two octaves of periodic value noise on a lattice
// stretched across the wind) and stirred by an evolving 3D value-noise swirl,
// both advected downwind. Every constant that would need a division arrives
// precomputed from the CPU (WindFieldGpu.scales), so both sides evaluate the
// same float operations; tests compare them. Keep the two in sync.

// Mirrors engine::renderer::WindFieldGpu (std140 / push constants, 64 bytes).
public struct WindFieldGpu {
    public float4 flow;   // xy unit heading (XZ), z mean speed m/s, w gustiness
    public float4 layers; // x turbulence, y gust offset, z swirl offset, w swirl phase (wrapped)
    public float4 scales; // x 1 / gust cell along, y 1 / gust cell across, z 1 / swirl cell, w unused
    public float4 grid;   // xy world XZ of texel (0, 0)'s corner, z texel size m, w unused
}

static const uint WIND_WRAP = 1023u; // lattice period 1024 (k_wind_lattice_period)

// pcg3d (Jarzynski & Olano 2020).
uint3 wind_pcg3d(uint3 v) {
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

float wind_lattice(uint x, uint y, uint z, uint seed) {
    uint3 h = wind_pcg3d(uint3(x & WIND_WRAP, y & WIND_WRAP, (z & WIND_WRAP) + seed * 1024u));
    return float(h.x >> 8u) * (1.0 / 16777216.0);
}

uint wind_cell(float floored) { return uint(int(floored)); }

float wind_fade(float t) { return t * t * (3.0 - 2.0 * t); }

float wind_mix(float a, float b, float t) { return a * (1.0 - t) + b * t; }

float wind_value_noise2(float2 p, uint seed) {
    float2 i = floor(p);
    float2 f = p - i;
    uint x0 = wind_cell(i.x);
    uint y0 = wind_cell(i.y);
    float a = wind_lattice(x0, y0, 0u, seed);
    float b = wind_lattice(x0 + 1u, y0, 0u, seed);
    float c = wind_lattice(x0, y0 + 1u, 0u, seed);
    float d = wind_lattice(x0 + 1u, y0 + 1u, 0u, seed);
    float u = wind_fade(f.x);
    float v = wind_fade(f.y);
    return wind_mix(wind_mix(a, b, u), wind_mix(c, d, u), v);
}

float wind_value_noise3(float3 p, uint seed) {
    float3 i = floor(p);
    float3 f = p - i;
    uint x0 = wind_cell(i.x);
    uint y0 = wind_cell(i.y);
    uint z0 = wind_cell(i.z);
    float u = wind_fade(f.x);
    float v = wind_fade(f.y);
    float w = wind_fade(f.z);
    float lo = wind_mix(
        wind_mix(wind_lattice(x0, y0, z0, seed), wind_lattice(x0 + 1u, y0, z0, seed), u),
        wind_mix(wind_lattice(x0, y0 + 1u, z0, seed), wind_lattice(x0 + 1u, y0 + 1u, z0, seed), u),
        v);
    float hi = wind_mix(
        wind_mix(wind_lattice(x0, y0, z0 + 1u, seed), wind_lattice(x0 + 1u, y0, z0 + 1u, seed), u),
        wind_mix(wind_lattice(x0, y0 + 1u, z0 + 1u, seed),
                 wind_lattice(x0 + 1u, y0 + 1u, z0 + 1u, seed), u),
        v);
    return wind_mix(lo, hi, w);
}

// The field at world (x, z), reference height: xyz = velocity m/s (y up),
// w = gust multiplier on the mean wind.
public float4 wind_field_sample(WindFieldGpu p, float2 xz) {
    float2 dir = p.flow.xy;
    float speed = p.flow.z;
    float gustiness = p.flow.w;
    float turbulence = p.layers.x;
    float along = xz.x * dir.x + xz.y * dir.y;
    float across = xz.y * dir.x - xz.x * dir.y;

    float2 g = float2(along * p.scales.x - p.layers.y, across * p.scales.y);
    float n = 0.65 * wind_value_noise2(g, 1u) +
              0.35 * wind_value_noise2(g * 2.0 + float2(17.0, 29.0), 2u);
    float t = clamp((n - 0.5) * 3.5714286, 0.0, 1.0);
    float front = t * t * (3.0 - 2.0 * t);
    float gust = 1.0 + gustiness * (1.6 * front - 0.35);

    float3 q = float3(along * p.scales.z - p.layers.z, across * p.scales.z, p.layers.w);
    float sx = wind_value_noise3(q, 3u) * 2.0 - 1.0;
    float sz = wind_value_noise3(q, 4u) * 2.0 - 1.0;
    float sv = wind_value_noise3(q, 5u) * 2.0 - 1.0;
    float swirl = speed * turbulence * (0.6 + 0.4 * gust);

    float h_along = speed * gust + swirl * 0.45 * sx;
    float h_across = swirl * 0.45 * sz;
    float3 velocity = float3(dir.x * h_along - dir.y * h_across, speed * turbulence * 0.12 * sv,
                             dir.y * h_along + dir.x * h_across);
    return float4(velocity, gust);
}

// The wind texture WindFieldPass writes (rgba16f): r = velocity x, g =
// velocity z, b = velocity y (m/s at the reference height), a = gust. `rect`
// = (world X, world Z of the texture's corner, 1 / covered size in metres,
// unused). Sampled bilinearly, clamped at the edges.
public float4 wind_texture_sample(Texture2D tex, SamplerState samp, float4 rect, float2 xz) {
    float2 uv = (xz - rect.xy) * rect.z;
    return tex.SampleLevel(samp, uv, 0.0);
}
