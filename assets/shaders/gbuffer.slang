// gbuffer.slang — deferred GBuffer pass (Phase 3, Slice 3.5) with the built-in
// material.
//
// The pass itself (GBuffer targets, R11 velocity, instancing, glTF
// doubleSided / mirrored-instance normals) lives in lib/gbuffer_common.slang;
// this program specialises it for StandardMaterial (glTF metallic-roughness
// PBR through the bound PbrMaterial parameter block). Authored material
// shaders get the same pass from lib/material_pass_gbuffer.slang.
//
// Two fragment entry points share one body, specialised on the alpha mode
// (lib/alpha_test.slang): fragment_main for opaque materials (no discard, so
// the pipeline keeps early depth testing) and fragment_masked for glTF MASK
// materials (discards below alphaCutoff; R12.11). Both write the same targets
// (alpha_variant_shader_test checks this on the SPIR-V).

import alpha_test;
import gbuffer_common;

[shader("vertex")]
GBufferVsOut vertex_main(GBufferVsIn input) {
    return gbuffer_vertex(input);
}

GBufferOut gbuffer_shade<A : IAlphaMode>(GBufferVsOut input, bool front_face) {
    SurfaceSample s = gbuffer_evaluate<StandardMaterial>(input);
    AlphaTestInput alpha_input;
    alpha_input.object_pos = input.object_pos;
    alpha_input.seed = pc.alpha_hash;
    A::test(s.alpha, g_material.params.alpha_cutoff, alpha_input);
    return gbuffer_pack(input, s, front_face);
}

[shader("fragment")]
GBufferOut fragment_main(GBufferVsOut input, bool front_face : SV_IsFrontFace) {
    return gbuffer_shade<AlphaOpaque>(input, front_face);
}

[shader("fragment")]
GBufferOut fragment_masked(GBufferVsOut input, bool front_face : SV_IsFrontFace) {
    return gbuffer_shade<AlphaMasked>(input, front_face);
}
