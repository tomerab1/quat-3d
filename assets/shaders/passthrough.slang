// passthrough.slang — Slang compilation smoke test (Phase 2, Slice 2.1).
//
// A fullscreen-triangle vertex shader (positions/UVs generated from the vertex
// index, no vertex buffer) and a fragment shader that outputs a solid colour.
// Used to verify the slangc -> SPIR-V + reflection-json toolchain.

struct VertexOutput {
    float4 position : SV_Position;
    float2 uv       : TEXCOORD0;
};

[shader("vertex")]
VertexOutput vertex_main(uint vertex_id : SV_VertexID) {
    // Oversized triangle covering the whole screen.
    const float2 uv = float2((vertex_id << 1) & 2, vertex_id & 2);

    VertexOutput out;
    out.uv = uv;
    out.position = float4(uv * 2.0 - 1.0, 0.0, 1.0);
    return out;
}

[shader("fragment")]
float4 fragment_main(VertexOutput input) : SV_Target {
    return float4(1.0, 0.5, 0.2, 1.0);
}
