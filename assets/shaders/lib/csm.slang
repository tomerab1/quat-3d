// csm.slang — cascaded shadow maps for the sun (ROADMAP R12).
//
// Two halves:
//   * Fitting (csm_setup.slang): the GPU port of engine/renderer/csm.cpp,
//     operation for operation — practical splits over the frame's depth
//     range, bounding-sphere cascades with power-of-two-quantised radius,
//     centres snapped to whole texels in a camera-independent light basis.
//   * Sampling (lighting.slang): cascade selection by view distance with a
//     cross-faded blend band, normal + light-direction receiver offsets in
//     texel units, PCF over a noise-rotated Vogel disk of hardware 2x2
//     comparison taps, and an optional PCSS penumbra (Fernando 2005: blocker
//     search, penumbra = receiver-blocker distance * sun angular size).
//
// Matrices are stored as four glm columns (cluster_mul convention) so the
// layout never depends on a matrix-layout qualifier.

public static const uint CSM_MAX_CASCADES = 4;

// Mirrors engine::renderer::GpuCascades (320 bytes).
public struct CascadeData {
    public float4 view_proj[16]; // cascade c: columns [4c, 4c + 3]
    public float4 split_far;     // view distance where each cascade ends
    public float4 texel_world;   // world size of one shadow texel
    public float4 depth_range;   // world distance spanned by depth 0..1
    public float4 params;        // x count, y first split, z blend band, w fitted to bounds
}

public float4 csm_mul(CascadeData d, uint c, float4 v) {
    return d.view_proj[c * 4] * v.x + d.view_proj[c * 4 + 1] * v.y +
           d.view_proj[c * 4 + 2] * v.z + d.view_proj[c * 4 + 3] * v.w;
}

// ---------------------------------------------------------------------------
// Fitting
// ---------------------------------------------------------------------------

// 2^(floor(log2 v) - bits) and its reciprocal, from the exponent bits (exact,
// matching std::frexp / std::ldexp on the CPU).
float octave_step(float v, int bits) {
    int e = int((asuint(v) >> 23) & 0xFFu) - 127;
    return asfloat(uint(e - bits + 127) << 23);
}
float octave_step_rcp(float v, int bits) {
    int e = int((asuint(v) >> 23) & 0xFFu) - 127;
    return asfloat(uint(127 - (e - bits)) << 23);
}

public float csm_quantize(float v, int bits, bool up) {
    if (!(v > 0.0)) {
        return 0.0;
    }
    float units = v * octave_step_rcp(v, bits);
    return (up ? ceil(units) : floor(units)) * octave_step(v, bits);
}

public float4 mul_cols(float4 cols[4], float4 v) {
    return cols[0] * v.x + cols[1] * v.y + cols[2] * v.z + cols[3] * v.w;
}

public float3 csm_unproject(float4 inv_proj[4], float x, float y, float z) {
    float4 p = mul_cols(inv_proj, float4(x, y, z, 1.0));
    return p.xyz / p.w;
}

public float csm_view_distance(float4 inv_proj[4], float depth) {
    return -csm_unproject(inv_proj, 0.0, 0.0, depth).z;
}

public struct LightBasis {
    public float3 side;
    public float3 up;
    public float3 forward; // direction the light travels
}

public LightBasis csm_light_basis(float3 to_light) {
    LightBasis b;
    b.forward = -normalize(to_light);
    float3 world_up = abs(b.forward.y) > 0.99 ? float3(0.0, 0.0, 1.0) : float3(0.0, 1.0, 0.0);
    b.side = normalize(cross(b.forward, world_up));
    b.up = cross(b.side, b.forward);
    return b;
}

public float csm_split(float near_dist, float far_dist, uint i, uint count, float lambda) {
    if (i == 0) {
        return near_dist;
    }
    if (i >= count) {
        return far_dist;
    }
    float k = float(i) / float(count);
    float log_split = near_dist * pow(far_dist / near_dist, k);
    float lin_split = near_dist + (far_dist - near_dist) * k;
    return lambda * log_split + (1.0 - lambda) * lin_split;
}

public struct CascadeFit {
    public float4 cols[4];
    public float  texel;
    public float  depth_range;
}

// Fits one cascade to the view-distance slice [start, end].
public CascadeFit csm_fit_slice(float4 inv_view[4], float4 inv_proj[4], LightBasis light,
                                float start, float end, float map_size, float caster_distance) {
    static const float2 ndc[4] = { float2(-1.0, -1.0), float2(1.0, -1.0), float2(-1.0, 1.0),
                                   float2(1.0, 1.0) };
    float3 corners[8];
    for (uint i = 0; i < 4; ++i) {
        float3 pn = csm_unproject(inv_proj, ndc[i].x, ndc[i].y, 0.0);
        float3 pf = csm_unproject(inv_proj, ndc[i].x, ndc[i].y, 1.0);
        float dn = -pn.z;
        float span = -pf.z - dn;
        float t0 = (start - dn) / span;
        float t1 = (end - dn) / span;
        corners[i] = mul_cols(inv_view, float4(lerp(pn, pf, t0), 1.0)).xyz;
        corners[i + 4] = mul_cols(inv_view, float4(lerp(pn, pf, t1), 1.0)).xyz;
    }
    float3 centre = float3(0.0, 0.0, 0.0);
    for (uint i = 0; i < 8; ++i) {
        centre += corners[i];
    }
    centre *= 0.125;
    float radius = 0.0;
    for (uint i = 0; i < 8; ++i) {
        radius = max(radius, length(corners[i] - centre));
    }
    radius = csm_quantize(radius * (map_size / (map_size - 2.0)), 4, true);

    float texel = 2.0 * radius / map_size;
    float lx = dot(light.side, centre);
    float ly = dot(light.up, centre);
    float lz = dot(light.forward, centre);
    float cx = floor(lx / texel) * texel;
    float cy = floor(ly / texel) * texel;
    float near_d = lz - radius - caster_distance;
    float far_d = lz + radius;
    float depth_span = far_d - near_d;

    float inv_r = 1.0 / radius;
    float inv_d = 1.0 / depth_span;
    float4 row0 = float4(light.side * inv_r, -cx * inv_r);
    float4 row1 = float4(light.up * inv_r, -cy * inv_r);
    float4 row2 = float4(light.forward * inv_d, -near_d * inv_d);
    CascadeFit f;
    f.cols[0] = float4(row0.x, row1.x, row2.x, 0.0);
    f.cols[1] = float4(row0.y, row1.y, row2.y, 0.0);
    f.cols[2] = float4(row0.z, row1.z, row2.z, 0.0);
    f.cols[3] = float4(row0.w, row1.w, row2.w, 1.0);
    f.texel = texel;
    f.depth_range = depth_span;
    return f;
}

// ---------------------------------------------------------------------------
// Sampling
// ---------------------------------------------------------------------------

// Filtering parameters (the lighting constants' csm vector):
// x = PCF radius (texels), y = tan(sun angular diameter) (0 = no PCSS),
// z = normal bias (texels), w = depth bias (texels).
public struct CsmFilter {
    public float pcf_radius;
    public float light_tan;
    public float normal_bias;
    public float depth_bias;
}

static const uint  CSM_PCF_TAPS = 16;
static const uint  CSM_BLOCKER_TAPS = 12;
static const float CSM_MAX_FILTER_TEXELS = 24.0;
static const float CSM_GOLDEN_ANGLE = 2.39996323;

// Vogel disk sample k of n, radius 1, rotated by `phi`.
float2 vogel(uint k, uint n, float phi) {
    float r = sqrt((float(k) + 0.5) / float(n));
    float theta = float(k) * CSM_GOLDEN_ANGLE + phi;
    return r * float2(cos(theta), sin(theta));
}

// Fraction of the sun reaching `world_pos` through cascade `c`.
float sample_cascade(CascadeData d, uint c, Texture2DArray map, SamplerComparisonState cmp,
                     SamplerState point, float3 world_pos, float3 normal, float3 to_light,
                     CsmFilter f, float noise) {
    float texel = d.texel_world[c];
    float ndotl = saturate(dot(normal, to_light));
    float3 p = world_pos + normal * (f.normal_bias * texel * (1.0 - 0.5 * ndotl)) +
               to_light * (f.depth_bias * texel);
    float4 clip = csm_mul(d, c, float4(p, 1.0));
    float2 uv = clip.xy * 0.5 + 0.5;
    float z = clip.z;
    if (any(uv < 0.0) || any(uv > 1.0) || z > 1.0) {
        return 1.0;
    }
    uint w = 0;
    uint h = 0;
    uint layers = 0;
    map.GetDimensions(w, h, layers);
    float texel_uv = 1.0 / float(w);
    float phi = noise * 6.28318531;

    float radius = f.pcf_radius;
    if (f.light_tan > 0.0) {
        // Blocker search over the widest penumbra this cascade allows.
        float search = CSM_MAX_FILTER_TEXELS * 0.5;
        float blocker_sum = 0.0;
        float blockers = 0.0;
        for (uint k = 0; k < CSM_BLOCKER_TAPS; ++k) {
            float2 o = vogel(k, CSM_BLOCKER_TAPS, phi) * (search * texel_uv);
            float bd = map.SampleLevel(point, float3(uv + o, float(c)), 0.0).r;
            if (bd < z) {
                blocker_sum += bd;
                blockers += 1.0;
            }
        }
        if (blockers < 0.5) {
            return 1.0;
        }
        float blocker = blocker_sum / blockers;
        float penumbra = (z - blocker) * d.depth_range[c] * f.light_tan / texel;
        radius = clamp(penumbra, f.pcf_radius, CSM_MAX_FILTER_TEXELS);
    }
    if (radius < 0.5) {
        return map.SampleCmpLevelZero(cmp, float3(uv, float(c)), z);
    }
    float sum = 0.0;
    for (uint k = 0; k < CSM_PCF_TAPS; ++k) {
        float2 o = vogel(k, CSM_PCF_TAPS, phi) * (radius * texel_uv);
        sum += map.SampleCmpLevelZero(cmp, float3(uv + o, float(c)), z);
    }
    return sum / float(CSM_PCF_TAPS);
}

// The sun's visibility at `world_pos` (view distance `view_dist`), with the
// cascade it came from in `cascade` (CSM_MAX_CASCADES when beyond the last).
public float csm_visibility(CascadeData d, Texture2DArray map, SamplerComparisonState cmp,
                            SamplerState point, float3 world_pos, float3 normal,
                            float3 to_light, float view_dist, CsmFilter f, float noise,
                            out uint cascade) {
    uint count = min(uint(d.params.x), CSM_MAX_CASCADES);
    cascade = CSM_MAX_CASCADES;
    if (count == 0 || view_dist >= d.split_far[count - 1]) {
        return 1.0;
    }
    uint c = 0;
    while (c + 1 < count && view_dist >= d.split_far[c]) {
        ++c;
    }
    cascade = c;
    float vis = sample_cascade(d, c, map, cmp, point, world_pos, normal, to_light, f, noise);

    float start = c == 0 ? d.params.y : d.split_far[c - 1];
    float band_start = d.split_far[c] - d.params.z * (d.split_far[c] - start);
    if (view_dist > band_start) {
        float t = saturate((view_dist - band_start) / max(d.split_far[c] - band_start, 1e-4));
        // The last cascade fades out to unshadowed instead of into a next one.
        float next = c + 1 < count
                         ? sample_cascade(d, c + 1, map, cmp, point, world_pos, normal, to_light,
                                          f, noise)
                         : 1.0;
        vis = lerp(vis, next, t);
    }
    return vis;
}

// Debug tint per cascade (red, green, blue, yellow).
public float3 csm_debug_color(uint cascade) {
    static const float3 colors[4] = { float3(1.0, 0.35, 0.35), float3(0.35, 1.0, 0.35),
                                      float3(0.35, 0.5, 1.0), float3(1.0, 1.0, 0.35) };
    return cascade < CSM_MAX_CASCADES ? colors[cascade] : float3(1.0, 1.0, 1.0);
}
