// material_pass_gbuffer.slang — the GBuffer pass for an authored material
// shader (ROADMAP A6).
//
// A library module with entry points and one unresolved type: SurfaceMaterial.
// A material shader (assets/shaders/materials/<name>.slang) supplies it with
//     export struct SurfaceMaterial : IMaterial = <its struct>;
// and is linked against this module's cooked .slang-module in one slangc run
// (cmake/CompileShaders.cmake, rhi::ShaderCompiler), which yields
// cooked/shaders/materials/<name>.gbuffer.spv. Link-time specialisation: the
// material is resolved and inlined when the program is linked, so the result
// is the same code a hand-specialised pass would be, and the pipeline layout
// is exactly the built-in pass's (lib/gbuffer_common.slang).

import gbuffer_common;

extern struct SurfaceMaterial : IMaterial;

[shader("vertex")]
GBufferVsOut vertex_main(GBufferVsIn input) {
    return gbuffer_vertex(input);
}

// Authored materials may cut holes: alpha below 0.5 discards the fragment.
[shader("fragment")]
GBufferOut fragment_main(GBufferVsOut input, bool front_face : SV_IsFrontFace) {
    SurfaceSample s = gbuffer_evaluate<SurfaceMaterial>(input);
    if (s.alpha < 0.5) discard;
    return gbuffer_pack(input, s, front_face);
}
