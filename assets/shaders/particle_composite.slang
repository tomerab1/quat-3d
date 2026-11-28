// particle_composite.slang — resolves weighted blended OIT (McGuire & Bavoil,
// JCGT 2013) over the HDR (ROADMAP R10.3). A fullscreen triangle reads the
// accumulation (sum of w * premultiplied colour, sum of w * alpha) and the
// revealage (product of 1 - alpha) and outputs the weighted average colour
// with alpha 1 - revealage, blended SRC_ALPHA, ONE_MINUS_SRC_ALPHA.

Texture2D<float4> g_accum;
Texture2D<float>  g_reveal;

struct VsOut {
    float4 position : SV_Position;
}

[shader("vertex")]
VsOut vertex_main(uint vertex_id : SV_VertexID) {
    float2 uv = float2(float((vertex_id << 1u) & 2u), float(vertex_id & 2u));
    VsOut o;
    o.position = float4(uv * 2.0 - 1.0, 0.0, 1.0);
    return o;
}

[shader("fragment")]
float4 fragment_main(VsOut input) : SV_Target0 {
    int3 px = int3(int2(input.position.xy), 0);
    float reveal = g_reveal.Load(px);
    if (reveal >= 0.9999) {
        discard; // nothing transparent covers this pixel
    }
    float4 accum = g_accum.Load(px);
    // Overflowing half floats: fall back to the alpha sum (paper, sec. 5).
    if (any(isinf(accum.rgb))) {
        accum.rgb = accum.aaa;
    }
    float3 average = accum.rgb / max(accum.a, 1e-5);
    return float4(average, 1.0 - reveal);
}
