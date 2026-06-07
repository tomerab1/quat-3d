// mesh.slang — first triangle (Phase 2, Slice 2.5).
//
// A hardcoded triangle: three clip-space positions and per-vertex colours
// generated from SV_VertexID (no vertex buffer yet). The fragment shader
// interpolates the vertex colour. Real vertex-buffer-driven meshes arrive with
// the mesh pass in Phase 3.

struct VertexOutput {
    float4 position : SV_Position;
    float3 color    : COLOR0;
};

[shader("vertex")]
VertexOutput vertex_main(uint vertex_id : SV_VertexID) {
    // Clip-space corners of a centred triangle (CCW), and an RGB colour per
    // corner so the interpolated result is a smooth red/green/blue gradient.
    const float2 positions[3] = {
        float2( 0.0, -0.6),
        float2( 0.6,  0.6),
        float2(-0.6,  0.6),
    };
    const float3 colors[3] = {
        float3(1.0, 0.0, 0.0),
        float3(0.0, 1.0, 0.0),
        float3(0.0, 0.0, 1.0),
    };

    VertexOutput out;
    out.position = float4(positions[vertex_id], 0.0, 1.0);
    out.color = colors[vertex_id];
    return out;
}

[shader("fragment")]
float4 fragment_main(VertexOutput input) : SV_Target {
    return float4(input.color, 1.0);
}
