// terrain_synth.slang — GPU terrain synthesis (ROADMAP R10.2).
//
// Generates one terrain tile's height field (TerrainSynthPass): the noise
// field, then optional grid erosion, then the edge-faded blend, in metres.
// Entry points, one dispatch each over the tile's texels (8x8 groups):
//
//   synth_init       noise field (terrain_noise.slang) -> g_out (the
//                    un-eroded base) and g_height; zeroes the simulation state
//   erode_flux       \
//   erode_water       | one grid-erosion step: the four passes of
//   erode_apply       | src/terrain/grid_erosion.cpp, op for op
//   erode_transport  /
//   synth_finalize   g_out = base + fade * (eroded + settled sediment - base)
//
// Every dispatch writes only its own texel and reads state no texel of the
// same dispatch writes (sediment advection ping-pongs between the halves of
// g_sediment, selected by pc.cur), so the result is deterministic. The pass
// records a compute -> compute memory barrier between dispatches.
//
// Seams: the noise position of texel (x, z) is float(lattice index) * scale,
// the lattice index being world_tile * (resolution - 1) + (x, z) — integer
// math and one correctly rounded multiply, so a tile's edge column equals its
// neighbour's (and, with the same rounding, the CPU reference's) exactly.
// Erosion is faded to zero on the border band, which keeps the base exactly.

import terrain_noise;

// Mirrors TerrainSynthPass's SynthUniforms (std140, 128 bytes).
struct SynthParams {
    int4   tile;  // world tile x, z; resolution; octaves
    float4 noise; // noise texel scale, base frequency, lacunarity, gain
    float4 shape; // warp strength, height (m), -, -
    float4 step0; // dt, rain * dt, pipe, cell area
    float4 step1; // 1 / area, cell (m), 1 / cell, cell * sqrt(2)
    float4 step2; // advect (dt / cell), Ks * dt, Kd * dt, 1 - Ke * dt
    float4 step3; // thermal rate / 8, 1 / max depth, capacity, min tilt
    float4 step4; // talus, -, -, -
};

struct Push {
    uint cur; // sediment half holding the current state (0 or 1)
};

[[vk::binding(0)]] ConstantBuffer<SynthParams> g_params;
[[vk::binding(1)]] StructuredBuffer<uint>      g_perm;     // 512: permutation, twice
[[vk::binding(2)]] RWStructuredBuffer<float>   g_height;   // eroding terrain, m
[[vk::binding(3)]] RWStructuredBuffer<float>   g_water;    // water depth, m
[[vk::binding(4)]] RWStructuredBuffer<float>   g_sediment; // 2 x n: suspended sediment, m
[[vk::binding(5)]] RWStructuredBuffer<float4>  g_flux;     // outflow L (-x), R (+x), T (-z), B (+z)
[[vk::binding(6)]] RWStructuredBuffer<float4>  g_flow;     // u, v, sin(tilt), thermal delta
[[vk::binding(7)]] RWStructuredBuffer<float>   g_out;      // base field, then the result, m

[[vk::push_constant]] Push pc;

static const float k_min_flow_depth = 1e-4; // grid_erosion.cpp k_min_flow_depth

struct BufferPermutation : IPermutation {
    uint at(uint index) { return g_perm[index]; }
};

int resolution() { return g_params.tile.z; }
int last_texel() { return g_params.tile.z - 1; }
uint texel(int x, int z) { return uint(z * g_params.tile.z + x); }
uint texel_count() { return uint(g_params.tile.z * g_params.tile.z); }

bool inside(uint3 id) {
    return int(id.x) < resolution() && int(id.y) < resolution();
}

[shader("compute")]
[numthreads(8, 8, 1)]
void synth_init(uint3 id : SV_DispatchThreadID) {
    if (!inside(id)) return;
    SynthParams p = g_params;
    int2 g = p.tile.xy * (p.tile.z - 1) + int2(id.xy); // world texel lattice
    float scale = p.noise.x;
    float2 pos = float2(float(g.x) * scale, float(g.y) * scale);

    NoiseShape s;
    s.octaves = p.tile.w;
    s.lacunarity = p.noise.z;
    s.gain = p.noise.w;
    s.base_frequency = p.noise.y;
    s.warp_strength = p.shape.x;
    BufferPermutation perm;
    float metres = compose_height(perm, pos, s) * p.shape.y;

    uint i = texel(int(id.x), int(id.y));
    g_out[i] = metres;
    g_height[i] = metres;
    g_water[i] = 0.0;
    g_sediment[i] = 0.0;
    g_sediment[texel_count() + i] = 0.0;
    g_flux[i] = float4(0.0, 0.0, 0.0, 0.0);
    g_flow[i] = float4(0.0, 0.0, 0.0, 0.0);
}

// 1. Outflow flux from the water-surface differences.
float outflow(float h, int nx, int nz, float f) {
    SynthParams p = g_params;
    uint j = texel(nx, nz);
    float hn = g_height[j] + (g_water[j] + p.step0.y);
    return max(0.0, f + p.step0.z * (h - hn));
}

[shader("compute")]
[numthreads(8, 8, 1)]
void erode_flux(uint3 id : SV_DispatchThreadID) {
    if (!inside(id)) return;
    SynthParams p = g_params;
    int x = int(id.x);
    int z = int(id.y);
    int last = last_texel();
    uint i = texel(x, z);
    float d1 = g_water[i] + p.step0.y;
    float h = g_height[i] + d1;
    float4 f = g_flux[i];
    float4 o = float4(x > 0 ? outflow(h, x - 1, z, f.x) : 0.0,
                      x < last ? outflow(h, x + 1, z, f.y) : 0.0,
                      z > 0 ? outflow(h, x, z - 1, f.z) : 0.0,
                      z < last ? outflow(h, x, z + 1, f.w) : 0.0);
    float sum = o.x + o.y + o.z + o.w;
    // Never drain more water than the cell holds.
    float k = sum > 0.0 ? min(1.0, d1 * p.step0.w / (sum * p.step0.x)) : 1.0;
    g_flux[i] = o * k;
}

// 2. Water depth, velocity, slope and thermal transfer.
[shader("compute")]
[numthreads(8, 8, 1)]
void erode_water(uint3 id : SV_DispatchThreadID) {
    if (!inside(id)) return;
    SynthParams p = g_params;
    int x = int(id.x);
    int z = int(id.y);
    int last = last_texel();
    uint i = texel(x, z);
    float dt = p.step0.x;
    float cell = p.step1.y;
    float d1 = g_water[i] + p.step0.y;
    float4 f = g_flux[i];
    float in_l = x > 0 ? g_flux[texel(x - 1, z)].y : 0.0;
    float in_r = x < last ? g_flux[texel(x + 1, z)].x : 0.0;
    float in_t = z > 0 ? g_flux[texel(x, z - 1)].w : 0.0;
    float in_b = z < last ? g_flux[texel(x, z + 1)].z : 0.0;
    float in_sum = in_l + in_r + in_t + in_b;
    float out_sum = f.x + f.y + f.z + f.w;
    float d2 = max(0.0, d1 + dt * (in_sum - out_sum) * p.step1.x);

    float wx = (in_l - f.x + f.y - in_r) * 0.5;
    float wz = (in_t - f.z + f.w - in_b) * 0.5;
    float depth = 0.5 * (d1 + d2);
    bool flows = depth > k_min_flow_depth;
    float u = flows ? wx / (cell * depth) : 0.0;
    float v = flows ? wz / (cell * depth) : 0.0;

    int xl = max(x - 1, 0);
    int xr = min(x + 1, last);
    int zt = max(z - 1, 0);
    int zb = min(z + 1, last);
    float sx = (xr - xl) == 2 ? 0.5 : 1.0;
    float sz = (zb - zt) == 2 ? 0.5 : 1.0;
    float gx = (g_height[texel(xr, z)] - g_height[texel(xl, z)]) * (sx * p.step1.z);
    float gz = (g_height[texel(x, zb)] - g_height[texel(x, zt)]) * (sz * p.step1.z);
    float g2 = gx * gx + gz * gz;
    float sin_tilt = sqrt(g2 / (1.0 + g2));

    // Thermal weathering: a symmetric pairwise transfer of the height
    // difference beyond the talus slope (mass-conserving).
    float td = 0.0;
    float bi = g_height[i];
    for (int dz = -1; dz <= 1; ++dz) {
        for (int dx = -1; dx <= 1; ++dx) {
            int nx = x + dx;
            int nz = z + dz;
            if ((dx == 0 && dz == 0) || nx < 0 || nz < 0 || nx > last || nz > last) {
                continue;
            }
            float dist = (dx != 0 && dz != 0) ? p.step1.w : cell;
            float dh = g_height[texel(nx, nz)] - bi;
            float excess = abs(dh) - p.step4.x * dist;
            if (excess > 0.0) td += dh > 0.0 ? excess : -excess;
        }
    }
    g_flow[i] = float4(u, v, sin_tilt, td * p.step3.x);
    g_water[i] = d2;
}

// 3. Erosion / deposition against the transport capacity.
[shader("compute")]
[numthreads(8, 8, 1)]
void erode_apply(uint3 id : SV_DispatchThreadID) {
    if (!inside(id)) return;
    SynthParams p = g_params;
    uint i = texel(int(id.x), int(id.y));
    uint si = pc.cur * texel_count() + i;
    float4 w = g_flow[i];
    float s = g_sediment[si];
    float h = g_height[i];
    float speed = sqrt(w.x * w.x + w.y * w.y);
    float ramp = clamp(g_water[i] * p.step3.y, 0.0, 1.0);
    float capacity = p.step3.z * max(w.z, p.step3.w) * speed * ramp;
    if (capacity > s) {
        float amount = p.step2.y * (capacity - s);
        h -= amount;
        s += amount;
    } else {
        float amount = p.step2.z * (s - capacity);
        h += amount;
        s -= amount;
    }
    g_height[i] = h + w.w;
    g_sediment[si] = s;
}

// 4. Semi-Lagrangian sediment transport (pc.cur -> the other half) + evaporation.
[shader("compute")]
[numthreads(8, 8, 1)]
void erode_transport(uint3 id : SV_DispatchThreadID) {
    if (!inside(id)) return;
    SynthParams p = g_params;
    int x = int(id.x);
    int z = int(id.y);
    int last = last_texel();
    uint i = texel(x, z);
    uint src = pc.cur * texel_count();
    uint dst = (pc.cur ^ 1u) * texel_count();
    float4 w = g_flow[i];
    float flast = float(last);
    float px = clamp(float(x) - w.x * p.step2.x, 0.0, flast);
    float pz = clamp(float(z) - w.y * p.step2.x, 0.0, flast);
    int x0 = int(px);
    int z0 = int(pz);
    int x1 = min(x0 + 1, last);
    int z1 = min(z0 + 1, last);
    float tx = px - float(x0);
    float tz = pz - float(z0);
    g_sediment[dst + i] =
        mix_ref(mix_ref(g_sediment[src + texel(x0, z0)], g_sediment[src + texel(x1, z0)], tx),
                mix_ref(g_sediment[src + texel(x0, z1)], g_sediment[src + texel(x1, z1)], tx),
                tz);
    g_water[i] *= p.step2.w;
}

// Blend the eroded field (plus the sediment still in suspension, which
// settles) over the base with the border fade; border texels keep the base.
[shader("compute")]
[numthreads(8, 8, 1)]
void synth_finalize(uint3 id : SV_DispatchThreadID) {
    if (!inside(id)) return;
    int x = int(id.x);
    int z = int(id.y);
    uint i = texel(x, z);
    float fade = grid_erosion_fade(x, z, resolution());
    if (fade <= 0.0) return;
    float base = g_out[i];
    float eroded = g_height[i] + g_sediment[pc.cur * texel_count() + i];
    g_out[i] = base + fade * (eroded - base);
}
