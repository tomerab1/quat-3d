// terrain_attributes.slang — per-tile surface attributes for the terrain
// material (ROADMAP R26.1), baked once when a tile is adopted.
//
//   attr_weights   g_wsum[i]  = total outflow weight of texel i (MFD)
//   attr_flow_init g_flow[0..n) = 1 (each texel's own rain)
//   attr_flow_step g_flow[next] = 1 + sum over the 8 neighbours j of
//                  g_flow[cur][j] x (share of j's outflow that reaches i)
//   attr_finalize  g_out.r = concavity * 0.5 + 0.5, g_out.g = flow wetness
//
// Flow is multiple-flow-direction accumulation (Freeman, "Calculating
// catchment area with divergent flow based on a regular grid", Computers &
// Geosciences 17(3), 1991; contour-length weights after Quinn et al. 1991):
// each texel passes its accumulated water to every lower neighbour in
// proportion to (tan slope)^1.1 x contour length. The steps are Jacobi
// sweeps, so N sweeps gather the catchment within N texels: enough for the
// gullies and hollows the material reads (the GPU erosion carves them at
// this scale), and the result is a deterministic function of the heights.
// Concavity is the mean height of rings at 2 and 6 texels minus the centre
// height, over the ring radius (positive in hollows, negative on crests).
// engine::renderer::terrain_attributes_reference is the CPU mirror the GPU
// test compares against.

struct Push {
    uint  resolution;   // texels per side
    float metres;       // per texel
    uint  cur;          // flow half holding the current sweep (0 or 1)
    uint  pad;
}
[[vk::push_constant]] Push pc;

Texture2D<float>            g_height;  // R32F metres
RWStructuredBuffer<float>   g_flow;    // 2 x n: accumulated flow, ping-pong halves
RWStructuredBuffer<float>   g_wsum;    // n: total outflow weight per texel
[[vk::image_format("rgba8")]]
RWTexture2D<float4>         g_out;

static const int2 k_offsets[8] = {int2(-1, -1), int2(0, -1), int2(1, -1), int2(-1, 0),
                                  int2(1, 0),   int2(-1, 1), int2(0, 1),  int2(1, 1)};

float height_at(int2 p) {
    const int last = int(pc.resolution) - 1;
    return g_height.Load(int3(clamp(p, int2(0, 0), int2(last, last)), 0));
}

bool inside(int2 p) {
    return all(p >= int2(0, 0)) && all(p < int2(int(pc.resolution), int(pc.resolution)));
}

uint index_of(int2 p) { return uint(p.y) * pc.resolution + uint(p.x); }

// Weight of the flow from texel `from` (height hf) to its neighbour at offset
// `o` (height ht): (tan slope)^1.1 x contour length (0.5 cardinal, 0.354
// diagonal).
float outflow_weight(float hf, float ht, int2 o) {
    const bool diagonal = o.x != 0 && o.y != 0;
    const float dist = pc.metres * (diagonal ? 1.41421356 : 1.0);
    const float drop = hf - ht;
    if (drop <= 0.0) return 0.0;
    return pow(drop / dist, 1.1) * (diagonal ? 0.35355339 : 0.5);
}

[shader("compute")]
[numthreads(8, 8, 1)]
void attr_weights(uint3 id : SV_DispatchThreadID) {
    const int2 p = int2(id.xy);
    if (!inside(p)) return;
    const float h = height_at(p);
    float sum = 0.0;
    for (int k = 0; k < 8; ++k) {
        const int2 q = p + k_offsets[k];
        if (inside(q)) sum += outflow_weight(h, height_at(q), k_offsets[k]);
    }
    g_wsum[index_of(p)] = sum;
}

[shader("compute")]
[numthreads(8, 8, 1)]
void attr_flow_init(uint3 id : SV_DispatchThreadID) {
    const int2 p = int2(id.xy);
    if (!inside(p)) return;
    g_flow[index_of(p)] = 1.0;
}

[shader("compute")]
[numthreads(8, 8, 1)]
void attr_flow_step(uint3 id : SV_DispatchThreadID) {
    const int2 p = int2(id.xy);
    if (!inside(p)) return;
    const uint n = pc.resolution * pc.resolution;
    const uint src = pc.cur * n;
    const uint dst = (1u - pc.cur) * n;
    const float h = height_at(p);
    float acc = 1.0;
    for (int k = 0; k < 8; ++k) {
        const int2 q = p + k_offsets[k];
        if (!inside(q)) continue;
        const uint qi = index_of(q);
        const float total = g_wsum[qi];
        if (total <= 0.0) continue;
        const float w = outflow_weight(height_at(q), h, -k_offsets[k]);
        acc += g_flow[src + qi] * (w / total);
    }
    g_flow[dst + index_of(p)] = acc;
}

float ring_mean(int2 p, int r) {
    return 0.125 * (height_at(p + int2(r, 0)) + height_at(p - int2(r, 0)) +
                    height_at(p + int2(0, r)) + height_at(p - int2(0, r)) +
                    height_at(p + int2(r, r)) + height_at(p - int2(r, r)) +
                    height_at(p + int2(r, -r)) + height_at(p - int2(r, -r)));
}

[shader("compute")]
[numthreads(8, 8, 1)]
void attr_finalize(uint3 id : SV_DispatchThreadID) {
    const int2 p = int2(id.xy);
    if (!inside(p)) return;
    const float h = height_at(p);
    const float c2 = (ring_mean(p, 2) - h) / (2.0 * pc.metres);
    const float c6 = (ring_mean(p, 6) - h) / (6.0 * pc.metres);
    const float concavity = clamp(2.5 * (c2 + c6), -1.0, 1.0);
    const float flow = g_flow[pc.cur * pc.resolution * pc.resolution + index_of(p)];
    const float wet = saturate(log2(max(flow, 1.0)) / 10.0);
    g_out[p] = float4(concavity * 0.5 + 0.5, wet, 0.0, 1.0);
}
