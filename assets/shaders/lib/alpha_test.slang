// alpha_test.slang — glTF alpha modes for the passes that write depth
// (ROADMAP R12.11, issue #35; hashed alpha R26.3).
//
// alphaMode MASK cuts a surface out where base-colour alpha x factor falls
// below alphaCutoff (glTF: alpha >= cutoff renders fully opaque). Every pass
// that rasterises a material's coverage — GBuffer (and so Hi-Z, GTAO and
// contact shadows, which read its depth), the cascaded shadow casters and the
// translucent shadow casters — is written once against IAlphaMode and
// specialised per pipeline: AlphaOpaque has no test at all (its SPIR-V holds
// no discard, so opaque pipelines keep early depth testing), AlphaMasked
// discards. The passes pick the pipeline from the material's
// MATERIAL_ALPHA_MASK flag.
//
// Masked textures carry coverage-preserving mips (Castaño 2010; engine/asset/
// alpha_coverage.hpp), so the cut-out keeps its area at every distance. With
// a temporal resolve (TAA) the test is hashed (R26.3): the alpha is first
// turned into the fraction of the pixel the cut-out covers — the
// screen-space-sharpened alpha of an anti-aliased alpha test (Golus,
// "Anti-aliased Alpha Test: The Esoteric Alpha to Coverage", 2017) — and the
// fragment survives when that coverage exceeds a stable per-pixel threshold
// (Wyman & McGuire, "Hashed Alpha Testing", I3D 2017): a hash of the
// object-space position quantised at pixel scale, blended between two
// scales and remapped to a uniform distribution, so the pattern sticks to
// the surface as it moves and does not swim. A per-frame seed decorrelates
// frames for TAA, which integrates the dither into smooth partial coverage:
// sub-pixel leaves resolve to a soft canopy instead of a speckle of
// single-pixel dots, and cut-out edges are anti-aliased. Shadow casters run
// the same coverage with a fixed seed (the PCF kernel integrates it), so a
// leaf shadows what it covers in the GBuffer. Without TAA the hash is off and
// the test is the plain cutoff (coverage >= 0.5 <=> alpha >= cutoff).

public struct AlphaTestInput {
    public float3 object_pos; // object-space position: the hash domain
    public float  seed;       // < 0: plain cutoff test; >= 0: hashed, seeded
}

public interface IAlphaMode {
    // Discards the fragment when it is cut out. Fragment stage only.
    static void test(float alpha, float cutoff, AlphaTestInput input);
}

public struct AlphaOpaque : IAlphaMode {
    public static void test(float alpha, float cutoff, AlphaTestInput input) {}
}

// Sin-free 3D hash to [0, 1) (Hoskins, "Hash without Sine", hash13): fast
// trig on some GPUs collapses frac(sin()) lattices.
float alpha_hash13(float3 p) {
    p = frac(p * 0.1031);
    p += dot(p, p.zyx + 31.32);
    return frac((p.x + p.y) * p.z);
}

// Hash scale in pixels (Wyman & McGuire: 1 hashes per pixel).
static const float ALPHA_HASH_SCALE = 1.0;

// Wyman & McGuire 2017, listing 3 (isotropic): a stable threshold in (0, 1].
public float hashed_alpha_threshold(float3 p, float seed) {
    const float max_deriv = max(length(ddx(p)), length(ddy(p)));
    const float pix_scale = 1.0 / max(ALPHA_HASH_SCALE * max_deriv, 1e-8);
    const float lg = log2(pix_scale);
    const float2 scales = float2(exp2(floor(lg)), exp2(ceil(lg)));
    const float3 offset = float3(seed * 7.13, seed * 3.71, seed * 5.37);
    const float2 h = float2(alpha_hash13(floor(scales.x * p) + offset),
                            alpha_hash13(floor(scales.y * p) + offset));
    const float t = frac(lg);
    const float x = (1.0 - t) * h.x + t * h.y;
    // The blend of two uniform hashes is not uniform: map it back through
    // its cumulative distribution.
    const float a = max(min(t, 1.0 - t), 1e-6);
    const float3 cases = float3(x * x / (2.0 * a * (1.0 - a)), (x - 0.5 * a) / (1.0 - a),
                                1.0 - (1.0 - x) * (1.0 - x) / (2.0 * a * (1.0 - a)));
    const float threshold = x < 1.0 - a ? (x < a ? cases.x : cases.y) : cases.z;
    return clamp(threshold, 1e-6, 1.0);
}

// Fraction of the pixel where alpha >= cutoff (alpha sharpened by its
// screen-space rate of change).
public float alpha_pixel_coverage(float alpha, float cutoff) {
    return saturate((alpha - cutoff) / max(fwidth(alpha), 1e-4) + 0.5);
}

public struct AlphaMasked : IAlphaMode {
    public static void test(float alpha, float cutoff, AlphaTestInput input) {
        if (input.seed < 0.0) {
            if (alpha < cutoff) {
                discard;
            }
            return;
        }
        const float coverage = alpha_pixel_coverage(alpha, cutoff);
        if (coverage < hashed_alpha_threshold(input.object_pos, input.seed)) {
            discard;
        }
    }
}
