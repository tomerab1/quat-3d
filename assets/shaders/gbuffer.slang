// gbuffer.slang — deferred GBuffer pass (Phase 3, Slice 3.5) with the built-in
// material.
//
// The pass itself (GBuffer targets, R11 velocity, instancing) lives in
// lib/gbuffer_common.slang; this program specialises
// it for StandardMaterial (glTF metallic-roughness PBR through the bound
// PbrMaterial parameter block). Authored material shaders get the same pass
// from lib/material_pass_gbuffer.slang.

import gbuffer_common;

[shader("vertex")]
GBufferVsOut vertex_main(GBufferVsIn input) {
    return gbuffer_vertex(input);
}

[shader("fragment")]
GBufferOut fragment_main(GBufferVsOut input) {
    return gbuffer_pack(input, gbuffer_evaluate<StandardMaterial>(input));
}
