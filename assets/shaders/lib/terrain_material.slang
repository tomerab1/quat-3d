// terrain_material.slang — the layered terrain material (ROADMAP R26.1).
//
// Evaluated by terrain.slang through ITerrainMaterial; the C++ data model and
// the CPU reference of the blend rules are engine/terrain/material.hpp —
// terrain_rule_coverage / terrain_layer_weights / terrain_height_blend below
// mirror rule_coverage / layer_weights / height_blend there operation for
// operation (the GPU test checks the shader against the reference).
//
// Per pixel:
//   1. rule coverage of every layer (slope, altitude, curvature, flow, noise
//      jitter), composited bottom to top, painted weights laid over them;
//   2. each covering layer's texture height, then height blending (Mittring,
//      "Finding Next Gen: CryEngine 2", SIGGRAPH 2007 course; Mishkinis,
//      "Advanced Terrain Texture Splatting", 2013): layers within
//      blend_depth of the highest height + weight share the point;
//   3. albedo / normal / roughness / AO of the surviving layers. Flat ground
//      uses one top-down projection, hex-tiled (Mikkelsen, "Practical
//      Real-Time Hex-Tiling", JCGT 11(2), 2022 — the production form of
//      Heitz & Neyret, "High-Performance By-Example Noise using a
//      Histogram-Preserving Blending Operator", HPG 2018) so no repeat is
//      visible; steep slopes cross-fade into triplanar mapping with whiteout
//      normal blending (Golus, "Normal Mapping for a Triplanar Shader",
//      2017), so cliffs do not stretch;
//   4. macro variation (large-scale brightness noise) and, with distance,
//      the layers' mean colour (their 1x1 mip) instead of the detail: no
//      tiling or moire far away, and four texture reads instead of dozens.

public static const uint TERRAIN_MAX_LAYERS = 8;

// Mirrors engine::renderer::TerrainLayerGpu (96 bytes).
public struct TerrainLayerGpu {
    public float4 tint_uv;   // rgb tint, w = 1 / uv_scale_m
    public float4 slope;     // lo, hi, softness (degrees), noise
    public float4 altitude;  // lo, hi, softness (m), 1 = relative to the snowline
    public float4 affinity;  // concavity, flow, strength, paint channel (-1 none, 0 r, 1 g, 2 b)
    public float4 surface;   // normal strength, roughness scale, height offset, array slice (-1: untextured)
    public float4 fallback;  // untextured colour (linear), roughness
}

// Mirrors engine::renderer::TerrainMaterialGpu (816 bytes).
public struct TerrainMaterialParams {
    public float4 global0;   // layer count, blend depth, triplanar start (deg), triplanar sharpness
    public float4 global1;   // macro strength, 1 / macro scale m, far start m, 1 / far range m
    public float4 global2;   // stochastic tiling (0/1), snowline m, mean-colour mip, unused
    public TerrainLayerGpu layers[TERRAIN_MAX_LAYERS];
}

// What the material reads at one ground point.
public struct TerrainSurfaceInput {
    public float3 world_pos;
    public float3 normal;        // geometric (heightmap) normal
    public float  slope_deg;
    public float  concavity;     // -1 ridge .. 1 gully
    public float  flow;          // 0 dry .. 1 water collects
    public float4 paint;         // edit paint channels (rgb)
    public float  view_distance; // m
    public float3 dpdx;          // world position derivatives (texture gradients)
    public float3 dpdy;
}

public struct TerrainSurface {
    public float3 albedo;
    public float3 normal;
    public float  roughness;
    public float  occlusion;
}

// Where a layer's textures come from: the cooked texture arrays, or the
// tinted procedural fallback of an untextured layer.
public struct LayerTexel {
    public float3 albedo;
    public float2 normal_xy; // tangent-space X / Y in [-1, 1]
    public float  roughness;
    public float  occlusion;
    public float  height;    // 0..1
}

// The material's data and textures. Layers are read one element at a time
// (never the whole TerrainMaterialParams by value): SPIRV-Cross turns a copy
// of the constant buffer's layer array into an spvUnsafeArray copy that the
// Metal compiler rejects (MoltenVK), so nothing here copies the array.
public interface ITerrainLayerSource {
    // TerrainMaterialParams.global0 .. global2 (index 0..2), with this
    // frame's values (global2.y = snowline) filled in.
    float4 global(uint index);
    // TerrainMaterialParams.layers[index].
    TerrainLayerGpu layer(uint index);
    // Full-detail sample of layer `l` at `uv` (texture repeats) with gradients.
    LayerTexel sample(TerrainLayerGpu l, float2 uv, float2 duvdx, float2 duvdy);
    // The layer's mean texel (its smallest mip).
    LayerTexel mean(TerrainLayerGpu l);
}

public interface ITerrainMaterial {
    TerrainSurface evaluate(TerrainSurfaceInput s);
}

// --------------------------------------------------------------------------
// Noise (sin-free hash: NVIDIA's fast trig collapses frac(sin()) lattices).
// --------------------------------------------------------------------------
public float terrain_hash(float2 p) {
    float3 q = frac(float3(p.x, p.y, p.x) * 0.1031);
    q += dot(q, float3(q.y, q.z, q.x) + 33.33);
    return frac((q.x + q.y) * q.z);
}

public float2 terrain_hash2(float2 p) {
    float3 q = frac(float3(p.x, p.y, p.x) * float3(0.1031, 0.1030, 0.0973));
    q += dot(q, float3(q.y, q.z, q.x) + 33.33);
    return frac((float2(q.x, q.x) + float2(q.y, q.z)) * float2(q.z, q.y));
}

public float terrain_value_noise(float2 p) {
    float2 i = floor(p);
    float2 f = frac(p);
    float2 u = f * f * (3.0 - 2.0 * f);
    float a = terrain_hash(i);
    float b = terrain_hash(i + float2(1, 0));
    float c = terrain_hash(i + float2(0, 1));
    float d = terrain_hash(i + float2(1, 1));
    return lerp(lerp(a, b, u.x), lerp(c, d, u.x), u.y);
}

public float terrain_fbm(float2 p) {
    const float2x2 rot = float2x2(0.80, 0.60, -0.60, 0.80);
    float v = 0.0;
    float amp = 0.5;
    for (int i = 0; i < 4; ++i) {
        v += amp * terrain_value_noise(p);
        p = mul(rot, p) * 2.13 + float2(7.3, 3.1);
        amp *= 0.5;
    }
    return v / 0.9375; // 0..1
}

// --------------------------------------------------------------------------
// Blend rules (mirror of engine/terrain/material.cpp).
// --------------------------------------------------------------------------
static const float OPEN_LOW_ALTITUDE = -99999.0;
static const float OPEN_HIGH_ALTITUDE = 99999.0;

float band(float x, float lo, float hi, float soft, bool open_lo, bool open_hi) {
    float s = max(soft, 1e-3);
    float rise = open_lo ? 1.0 : smoothstep(lo - s, lo + s, x);
    float fall = open_hi ? 1.0 : 1.0 - smoothstep(hi - s, hi + s, x);
    return rise * fall;
}

float affinity(float k, float s) {
    k = clamp(k, -1.0, 1.0);
    float target = k >= 0.0 ? s : 1.0 - s;
    return 1.0 + (target - 1.0) * abs(k);
}

// Noise jitter of the band edges at the point: 0..1.
public float terrain_rule_coverage(TerrainLayerGpu l, TerrainSurfaceInput s, float noise,
                                   float snowline) {
    float jitter = (noise * 2.0 - 1.0) * saturate(l.slope.w);
    float slope = s.slope_deg + jitter * 2.0 * l.slope.z;
    float height_ref = l.altitude.w > 0.5 ? snowline : 0.0;
    float altitude = s.world_pos.y - height_ref + jitter * 2.0 * l.altitude.z;

    float c = band(slope, l.slope.x, l.slope.y, l.slope.z, l.slope.x <= 0.0, l.slope.y >= 90.0);
    c *= band(altitude, l.altitude.x, l.altitude.y, l.altitude.z,
              l.altitude.x <= OPEN_LOW_ALTITUDE, l.altitude.y >= OPEN_HIGH_ALTITUDE);
    float shape = smoothstep(0.0, 0.5, l.affinity.x >= 0.0 ? s.concavity : -s.concavity);
    c *= affinity(abs(l.affinity.x), shape);
    c *= affinity(l.affinity.y, smoothstep(0.35, 0.75, s.flow));
    return saturate(c * saturate(l.affinity.z));
}

float paint_of(float channel, float4 paint) {
    if (channel < -0.5) return 0.0;
    if (channel < 0.5) return saturate(paint.r);
    if (channel < 1.5) return saturate(paint.g);
    return saturate(paint.b);
}

// Composited rule coverages (bottom to top) with painted weights over them.
public void terrain_layer_weights<S : ITerrainLayerSource>(S src, TerrainSurfaceInput s,
                                                          float noise,
                                                          out float w[TERRAIN_MAX_LAYERS]) {
    uint n = min(uint(src.global(0).x), TERRAIN_MAX_LAYERS);
    const float snowline = src.global(2).y;
    float remaining = 1.0;
    [unroll]
    for (int i = int(TERRAIN_MAX_LAYERS) - 1; i >= 1; --i) {
        w[i] = 0.0;
        if (uint(i) < n) {
            float c = terrain_rule_coverage(src.layer(uint(i)), s, noise, snowline);
            w[i] = c * remaining;
            remaining -= w[i];
        }
    }
    w[0] = n > 0 ? remaining : 0.0;

    float painted[TERRAIN_MAX_LAYERS];
    float paint_sum = 0.0;
    [unroll]
    for (uint i = 0; i < TERRAIN_MAX_LAYERS; ++i) {
        painted[i] = i < n ? paint_of(src.layer(i).affinity.w, s.paint) : 0.0;
        paint_sum += painted[i];
    }
    if (paint_sum > 0.0) {
        float cover = min(paint_sum, 1.0);
        float norm = 1.0 / max(paint_sum, 1.0);
        [unroll]
        for (uint i = 0; i < TERRAIN_MAX_LAYERS; ++i) {
            w[i] = w[i] * (1.0 - cover) + painted[i] * norm;
        }
    }
}

// Height blending: layers within `depth` of the highest height + weight.
public void terrain_height_blend(inout float w[TERRAIN_MAX_LAYERS], float h[TERRAIN_MAX_LAYERS],
                                 float depth) {
    float d = max(depth, 1e-3);
    float top = -1e30;
    [unroll]
    for (uint i = 0; i < TERRAIN_MAX_LAYERS; ++i) {
        if (w[i] > 0.0) top = max(top, h[i] + w[i]);
    }
    float sum = 0.0;
    [unroll]
    for (uint i = 0; i < TERRAIN_MAX_LAYERS; ++i) {
        w[i] = w[i] > 0.0 ? max(h[i] + w[i] - (top - d), 0.0) : 0.0;
        sum += w[i];
    }
    if (sum > 0.0) {
        [unroll]
        for (uint i = 0; i < TERRAIN_MAX_LAYERS; ++i) w[i] /= sum;
    }
}

// --------------------------------------------------------------------------
// Hex tiling (Mikkelsen 2022, listing 1, with the sin-free hash).
// --------------------------------------------------------------------------
static const float HEX_SCALE = 3.4641016; // 2 sqrt(3): one hex per texture repeat

void hex_triangle(float2 st, out float3 w, out int2 v1, out int2 v2, out int2 v3) {
    st *= HEX_SCALE;
    const float2x2 to_skewed = float2x2(1.0, -0.57735027, 0.0, 1.15470054);
    float2 skewed = mul(to_skewed, st);
    int2 base = int2(floor(skewed));
    float3 t = float3(frac(skewed), 0.0);
    t.z = 1.0 - t.x - t.y;
    float s = step(0.0, -t.z);
    float s2 = 2.0 * s - 1.0;
    w = float3(-t.z * s2, s - t.y * s2, s - t.x * s2);
    v1 = base + int2(int(s), int(s));
    v2 = base + int2(int(s), int(1.0 - s));
    v3 = base + int2(int(1.0 - s), int(s));
}

float2 hex_centre(int2 v) {
    const float2x2 inv_skew = float2x2(1.0, 0.5, 0.0, 1.0 / 1.15470054);
    return mul(inv_skew, float2(v)) / HEX_SCALE;
}

float2x2 hex_rotation(int2 v) {
    float angle = (terrain_hash(float2(v) + 0.37) * 2.0 - 1.0) * 3.14159265;
    float c = cos(angle);
    float s = sin(angle);
    return float2x2(c, -s, s, c);
}

// Contrast-preserving hex weights from each tile's height (Mikkelsen's
// luminance-weighted variant with height in place of luminance).
float3 hex_weights(float3 w, float3 heights) {
    float3 d = lerp(float3(1.0, 1.0, 1.0), heights, 0.6);
    float3 r = d * pow(max(w, 1e-5), 7.0);
    return r / max(r.x + r.y + r.z, 1e-6);
}

LayerTexel texel_lerp3(LayerTexel a, LayerTexel b, LayerTexel c, float3 w) {
    LayerTexel o;
    o.albedo = a.albedo * w.x + b.albedo * w.y + c.albedo * w.z;
    o.normal_xy = a.normal_xy * w.x + b.normal_xy * w.y + c.normal_xy * w.z;
    o.roughness = a.roughness * w.x + b.roughness * w.y + c.roughness * w.z;
    o.occlusion = a.occlusion * w.x + b.occlusion * w.y + c.occlusion * w.z;
    o.height = a.height * w.x + b.height * w.y + c.height * w.z;
    return o;
}

// acc + t * w.
LayerTexel texel_add(LayerTexel acc, LayerTexel t, float w) {
    return texel_lerp3(acc, t, t, float3(1.0, w, 0.0));
}

LayerTexel sample_hex<S : ITerrainLayerSource>(S src, TerrainLayerGpu l, float2 uv, float2 dx,
                                               float2 dy) {
    float3 w;
    int2 v1, v2, v3;
    hex_triangle(uv, w, v1, v2, v3);
    float2x2 r1 = hex_rotation(v1);
    float2x2 r2 = hex_rotation(v2);
    float2x2 r3 = hex_rotation(v3);
    float2 c1 = hex_centre(v1), c2 = hex_centre(v2), c3 = hex_centre(v3);
    float2 uv1 = mul(uv - c1, r1) + c1 + terrain_hash2(float2(v1));
    float2 uv2 = mul(uv - c2, r2) + c2 + terrain_hash2(float2(v2));
    float2 uv3 = mul(uv - c3, r3) + c3 + terrain_hash2(float2(v3));
    LayerTexel t1 = src.sample(l, uv1, mul(dx, r1), mul(dy, r1));
    LayerTexel t2 = src.sample(l, uv2, mul(dx, r2), mul(dy, r2));
    LayerTexel t3 = src.sample(l, uv3, mul(dx, r3), mul(dy, r3));
    // Rotated tiles carry rotated tangent frames: turn the normals back.
    t1.normal_xy = mul(r1, t1.normal_xy);
    t2.normal_xy = mul(r2, t2.normal_xy);
    t3.normal_xy = mul(r3, t3.normal_xy);
    return texel_lerp3(t1, t2, t3, hex_weights(w, float3(t1.height, t2.height, t3.height)));
}

// --------------------------------------------------------------------------
// The layered material.
// --------------------------------------------------------------------------
struct Projection {
    float3 plane_w; // x, y, z plane weights (sum 1)
    float  hex;     // 1: hex-tile the top-down plane
}

// One layer's texel through the projection; `world_n` receives the layer's
// world normal (whiteout-blended over the geometric normal).
LayerTexel sample_layer<S : ITerrainLayerSource>(S src, TerrainLayerGpu l, TerrainSurfaceInput s,
                                                 Projection p, out float3 world_n) {
    float scale = l.tint_uv.w;
    float3 n = s.normal;
    float3 sgn = float3(n.x >= 0.0 ? 1.0 : -1.0, n.y >= 0.0 ? 1.0 : -1.0, n.z >= 0.0 ? 1.0 : -1.0);
    float3 wp = s.world_pos * scale;
    float3 dx = s.dpdx * scale;
    float3 dy = s.dpdy * scale;

    LayerTexel acc = (LayerTexel)0;
    float3 nsum = float3(0.0, 0.0, 0.0);
    if (p.plane_w.x > 0.0) {
        float2 flip = float2(sgn.x, 1.0);
        LayerTexel t = src.sample(l, wp.zy * flip, dx.zy * flip, dy.zy * flip);
        float2 tn = t.normal_xy * l.surface.x * float2(sgn.x, 1.0);
        float3 tnx = float3(tn + n.zy, sqrt(saturate(1.0 - dot(t.normal_xy, t.normal_xy))) * n.x);
        nsum += tnx.zyx * p.plane_w.x;
        acc = texel_add(acc, t, p.plane_w.x);
    }
    if (p.plane_w.y > 0.0) {
        float2 flip = float2(sgn.y, 1.0);
        float2 uv = wp.xz * flip;
        LayerTexel t = p.hex > 0.5 ? sample_hex(src, l, uv, dx.xz * flip, dy.xz * flip)
                                   : src.sample(l, uv, dx.xz * flip, dy.xz * flip);
        float2 tn = t.normal_xy * l.surface.x * float2(sgn.y, 1.0);
        float3 tny = float3(tn + n.xz, sqrt(saturate(1.0 - dot(t.normal_xy, t.normal_xy))) * n.y);
        nsum += tny.xzy * p.plane_w.y;
        acc = texel_add(acc, t, p.plane_w.y);
    }
    if (p.plane_w.z > 0.0) {
        float2 flip = float2(-sgn.z, 1.0);
        LayerTexel t = src.sample(l, wp.xy * flip, dx.xy * flip, dy.xy * flip);
        float2 tn = t.normal_xy * l.surface.x * float2(-sgn.z, 1.0);
        float3 tnz = float3(tn + n.xy, sqrt(saturate(1.0 - dot(t.normal_xy, t.normal_xy))) * n.z);
        nsum += tnz * p.plane_w.z;
        acc = texel_add(acc, t, p.plane_w.z);
    }
    world_n = normalize(nsum + n * 1e-4);
    return acc;
}

public struct LayeredTerrainMaterial<S : ITerrainLayerSource> : ITerrainMaterial {
    public S source;

    public TerrainSurface evaluate(TerrainSurfaceInput s) {
        const float4 g0 = source.global(0);
        const float4 g1 = source.global(1);
        const float4 g2 = source.global(2);
        uint n = min(uint(g0.x), TERRAIN_MAX_LAYERS);

        // Break-up noise of the band edges: metre-scale over 10 m patches.
        float noise = terrain_fbm(s.world_pos.xz * 0.11 + 3.7);
        float w[TERRAIN_MAX_LAYERS];
        terrain_layer_weights(source, s, noise, w);

        // Projection: top-down below the triplanar slope, triplanar above.
        float tri = smoothstep(g0.z - 5.0, g0.z + 5.0, s.slope_deg);
        float3 pw = pow(abs(s.normal), g0.w);
        pw /= max(pw.x + pw.y + pw.z, 1e-5);
        pw = lerp(float3(0.0, 1.0, 0.0), pw, tri);
        pw = select(pw < 0.03, float3(0.0, 0.0, 0.0), pw);
        pw /= max(pw.x + pw.y + pw.z, 1e-5);
        Projection proj;
        proj.plane_w = pw;
        proj.hex = g2.x;

        // Far field: the layers' mean texel instead of the detail.
        float far = saturate((s.view_distance - g1.z) * g1.w);
        bool detail = far < 0.999;

        LayerTexel texel[TERRAIN_MAX_LAYERS];
        float3 normals[TERRAIN_MAX_LAYERS];
        float h[TERRAIN_MAX_LAYERS];
        [unroll]
        for (uint i = 0; i < TERRAIN_MAX_LAYERS; ++i) {
            texel[i] = (LayerTexel)0;
            normals[i] = s.normal;
            h[i] = 0.0;
            if (i < n && w[i] > 0.001) {
                TerrainLayerGpu l = source.layer(i);
                LayerTexel mean = source.mean(l);
                LayerTexel t = mean;
                if (detail) {
                    float3 ln;
                    t = sample_layer(source, l, s, proj, ln);
                    normals[i] = normalize(lerp(ln, s.normal, far));
                    t = texel_lerp3(t, mean, mean, float3(1.0 - far, far, 0.0));
                }
                t.albedo *= l.tint_uv.rgb;
                t.roughness *= l.surface.y;
                texel[i] = t;
                h[i] = saturate(t.height + l.surface.z);
            }
        }
        terrain_height_blend(w, h, g0.y);

        TerrainSurface o;
        o.albedo = float3(0.0, 0.0, 0.0);
        o.normal = float3(0.0, 0.0, 0.0);
        o.roughness = 0.0;
        o.occlusion = 0.0;
        [unroll]
        for (uint i = 0; i < TERRAIN_MAX_LAYERS; ++i) {
            o.albedo += texel[i].albedo * w[i];
            o.normal += normals[i] * w[i];
            o.roughness += texel[i].roughness * w[i];
            o.occlusion += texel[i].occlusion * w[i];
        }
        o.normal = normalize(o.normal + s.normal * 1e-4);

        // Macro variation: +-strength brightness over macro-scale patches.
        float macro = terrain_fbm(s.world_pos.xz * g1.y);
        o.albedo *= 1.0 + g1.x * (macro * 2.0 - 1.0);
        o.albedo = saturate(o.albedo);
        // Physically plausible ground: no rough natural surface goes glossy.
        o.roughness = clamp(o.roughness, 0.3, 1.0);
        o.occlusion = saturate(o.occlusion);
        return o;
    }
}
