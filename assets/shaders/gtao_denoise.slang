// gtao_denoise.slang — XeGTAO's edge-aware spatial denoiser (ROADMAP R12).
//
// Port of XeGTAO_Denoise (MIT, https://github.com/GameTechDev/XeGTAO): a 3x3
// blur whose cross taps are weighted by the packed depth edges gtao.slang
// wrote (made symmetric with the neighbours' opposite edges) and whose
// diagonals by the product of the two edges they cross; a little leakage
// is allowed on pixels with 3-4 edges to reduce aliasing. The visibility and
// the bent normal are filtered together. GtaoPass runs it 1-3 times; only
// the last pass uses the full blur beta (earlier ones a fifth), as in
// XeGTAO. Temporal accumulation is left to TAA.

// Mirrors GtaoDenoisePush in gtao_pass.cpp (8 bytes).
struct DenoiseParams {
    float blur_amount; // centre weight (DenoiseBlurBeta, / 5 before the last pass)
    float unused;
}
[[vk::push_constant]] DenoiseParams pc;

Texture2D                             g_ao_in;    // rgb bent normal * 0.5 + 0.5, a visibility
Texture2D                             g_edges;    // packed LRTB edges (r8)
[format("rgba8")] RWTexture2D<float4> g_ao_out;

float4 unpack_edges(float packed) {
    uint v = uint(packed * 255.5);
    return saturate(float4(float((v >> 6) & 3u), float((v >> 4) & 3u), float((v >> 2) & 3u),
                            float(v & 3u)) / 3.0);
}

// Decoded working term: xyz bent normal, w visibility.
float4 load_term(int2 p, int2 size) {
    float4 t = g_ao_in.Load(int3(clamp(p, int2(0, 0), size - 1), 0));
    return float4(t.rgb * 2.0 - 1.0, t.a);
}

float4 load_edges(int2 p, int2 size) {
    return unpack_edges(g_edges.Load(int3(clamp(p, int2(0, 0), size - 1), 0)).r);
}

[shader("compute")]
[numthreads(8, 8, 1)]
void compute_main(uint3 tid : SV_DispatchThreadID) {
    uint w = 0;
    uint h = 0;
    g_ao_out.GetDimensions(w, h);
    if (tid.x >= w || tid.y >= h) {
        return;
    }
    int2 pix = int2(tid.xy);
    int2 size = int2(int(w), int(h));

    float4 edges_c = load_edges(pix, size);
    float4 edges_l = load_edges(pix + int2(-1, 0), size);
    float4 edges_r = load_edges(pix + int2(1, 0), size);
    float4 edges_t = load_edges(pix + int2(0, -1), size);
    float4 edges_b = load_edges(pix + int2(0, 1), size);
    // Symmetric edges: a left edge here must match the right edge on the left.
    edges_c *= float4(edges_l.y, edges_r.x, edges_t.w, edges_b.z);

    const float leak_threshold = 2.5;
    const float leak_strength = 0.5;
    float edginess = (saturate(4.0 - leak_threshold - dot(edges_c, float4(1.0, 1.0, 1.0, 1.0))) /
                      (4.0 - leak_threshold)) * leak_strength;
    edges_c = saturate(edges_c + edginess);

    const float diag_weight = 0.85 * 0.5;
    float weight_tl = diag_weight * (edges_c.x * edges_l.z + edges_c.z * edges_t.x);
    float weight_tr = diag_weight * (edges_c.z * edges_t.y + edges_c.y * edges_r.z);
    float weight_bl = diag_weight * (edges_c.w * edges_b.x + edges_c.x * edges_l.w);
    float weight_br = diag_weight * (edges_c.y * edges_r.w + edges_c.w * edges_b.y);

    float blur = pc.blur_amount;
    float4 sum = load_term(pix, size) * blur;
    float weight = blur;
    sum += load_term(pix + int2(-1, 0), size) * edges_c.x;
    sum += load_term(pix + int2(1, 0), size) * edges_c.y;
    sum += load_term(pix + int2(0, -1), size) * edges_c.z;
    sum += load_term(pix + int2(0, 1), size) * edges_c.w;
    sum += load_term(pix + int2(-1, -1), size) * weight_tl;
    sum += load_term(pix + int2(1, -1), size) * weight_tr;
    sum += load_term(pix + int2(-1, 1), size) * weight_bl;
    sum += load_term(pix + int2(1, 1), size) * weight_br;
    weight += edges_c.x + edges_c.y + edges_c.z + edges_c.w + weight_tl + weight_tr +
              weight_bl + weight_br;

    float4 term = sum / weight;
    float3 bent = normalize(term.xyz + float3(0.0, 1e-6, 0.0));
    g_ao_out[pix] = float4(bent * 0.5 + 0.5, saturate(term.w));
}
