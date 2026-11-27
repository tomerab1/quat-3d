// alpha_test.slang — glTF alpha modes for the passes that write depth
// (ROADMAP R12.11, issue #35).
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
// The test itself is plain: masked textures carry coverage-preserving mips
// (engine/asset/alpha_coverage.hpp), so the cut-out keeps its area at every
// distance, and TAA resolves the sub-pixel edge.

public interface IAlphaMode {
    // Discards the fragment when it is cut out. Fragment stage only.
    static void test(float alpha, float cutoff);
}

public struct AlphaOpaque : IAlphaMode {
    public static void test(float alpha, float cutoff) {}
}

public struct AlphaMasked : IAlphaMode {
    public static void test(float alpha, float cutoff) {
        if (alpha < cutoff) {
            discard;
        }
    }
}
