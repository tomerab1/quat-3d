// terrain_noise.slang — the terrain height field on the GPU (ROADMAP R10.2).
//
// A line-for-line port of src/terrain/generator.cpp's GradientNoise, fbm,
// ridged_fbm and compose_height: seeded Perlin gradient noise, FBM, ridged
// FBM, iq domain warp and the continental plains/mountains blend. The CPU
// generator is the reference — keep the two in step, including the order of
// operations and glm's mix / smoothstep formulas (mix_ref / smoothstep_ref),
// so GPU and CPU heights agree to float rounding (GpuTerrainSynth tests).
//
// The permutation (the seed's 256-entry shuffle, duplicated to 512) comes
// from the caller through IPermutation, so the module binds no resources.

public interface IPermutation {
    public uint at(uint index);
}

// glm::mix: x * (1 - a) + y * a.
public float mix_ref(float x, float y, float a) {
    return x * (1.0 - a) + y * a;
}

// glm::smoothstep.
public float smoothstep_ref(float edge0, float edge1, float x) {
    float t = clamp((x - edge0) / (edge1 - edge0), 0.0, 1.0);
    return t * t * (3.0 - 2.0 * t);
}

static const float2 k_gradients[8] = {
    float2(1.0, 0.0),          float2(-1.0, 0.0),
    float2(0.0, 1.0),          float2(0.0, -1.0),
    float2(0.70710678, 0.70710678),  float2(-0.70710678, 0.70710678),
    float2(0.70710678, -0.70710678), float2(-0.70710678, -0.70710678),
};

float dot_gradient(uint h, float2 d) {
    float2 g = k_gradients[h & 7u];
    return g.x * d.x + g.y * d.y; // glm::dot's order
}

uint lattice_hash<P : IPermutation>(P perm, int x, int z) {
    return perm.at(perm.at(uint(x & 255)) + uint(z & 255));
}

// Value in [-1, 1].
public float gradient_noise<P : IPermutation>(P perm, float2 p) {
    float2 pf = floor(p);
    int xi = int(pf.x) & 255;
    int zi = int(pf.y) & 255;
    float2 f = p - pf;
    float2 u = f * f * (3.0 - 2.0 * f);

    float g00 = dot_gradient(lattice_hash(perm, xi, zi), f);
    float g10 = dot_gradient(lattice_hash(perm, xi + 1, zi), f - float2(1.0, 0.0));
    float g01 = dot_gradient(lattice_hash(perm, xi, zi + 1), f - float2(0.0, 1.0));
    float g11 = dot_gradient(lattice_hash(perm, xi + 1, zi + 1), f - float2(1.0, 1.0));
    return mix_ref(mix_ref(g00, g10, u.x), mix_ref(g01, g11, u.x), u.y) * 1.42;
}

public float fbm<P : IPermutation>(P perm, float2 p, int octaves, float lacunarity, float gain) {
    float value = 0.0;
    float amplitude = 0.5;
    float total = 0.0;
    for (int i = 0; i < octaves; ++i) {
        value += amplitude * gradient_noise(perm, p);
        total += amplitude;
        p *= lacunarity;
        amplitude *= gain;
    }
    return value / total;
}

public float ridged_fbm<P : IPermutation>(P perm, float2 p, int octaves, float lacunarity,
                                          float gain) {
    float value = 0.0;
    float amplitude = 0.5;
    float total = 0.0;
    float weight = 1.0;
    for (int i = 0; i < octaves; ++i) {
        float n = 1.0 - abs(gradient_noise(perm, p));
        n *= n * weight;
        weight = clamp(n * 2.0, 0.0, 1.0); // crests spawn finer crests
        value += amplitude * n;
        total += amplitude;
        p *= lacunarity;
        amplitude *= gain;
    }
    return value / total;
}

// Shape parameters of TerrainParams that the height field reads.
public struct NoiseShape {
    public int   octaves;
    public float lacunarity;
    public float gain;
    public float base_frequency;
    public float warp_strength;
}

// One texel of the composed height field, normalised [0, 1], at noise-space
// position `p` (world texel lattice index * noise texel scale).
public float compose_height<P : IPermutation>(P perm, float2 p, NoiseShape s) {
    // iq domain warp: offset the sample position by another FBM pair.
    float wx = fbm(perm, p + float2(5.2, 1.3), 4, s.lacunarity, s.gain);
    float wz = fbm(perm, p + float2(9.7, 8.3), 4, s.lacunarity, s.gain);
    float2 warped = p + (s.warp_strength * s.base_frequency) * float2(wx, wz);

    // Low-frequency continental mask decides where mountains rise.
    float continent =
        smoothstep_ref(-0.20, 0.45, fbm(perm, warped * 0.35, 4, s.lacunarity, s.gain));

    float plains = 0.5 + 0.5 * fbm(perm, warped, s.octaves, s.lacunarity, s.gain);
    float mountains = ridged_fbm(perm, warped * 1.7, s.octaves, s.lacunarity, s.gain);

    // Plains stay gentle (compressed), mountains use the full range.
    float h = mix_ref(0.18 + 0.22 * plains, 0.25 + 0.75 * mountains, continent);
    return clamp(h, 0.0, 1.0);
}

// Tile-local erosion weight at texel (x, z) (generator.cpp grid_erosion_fade):
// 0 on the outer border band, ramping to 1 inside.
public float grid_erosion_fade(int x, int z, int resolution) {
    int last = resolution - 1;
    int edge = min(min(x, z), min(last - x, last - z)) - 4;
    float band = max(float(resolution) * 0.05, 4.0);
    return clamp(float(edge) / band, 0.0, 1.0);
}
