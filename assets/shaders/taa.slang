// taa.slang — temporal anti-aliasing resolve (Phase 8.1).
//
// Reprojects the previous frame's resolved colour into the current frame using
// depth + the previous view-projection, clips it to the current 3x3 colour
// neighbourhood (AABB clamp, the standard ghosting suppressor), and blends it
// with the (camera-jittered) current frame. Accumulated over frames this gives
// temporal anti-aliasing. History is point/bilinear-reprojected per pixel.

[[vk::binding(0)]] Texture2D          g_current;
[[vk::binding(1)]] Texture2D          g_history;
[[vk::binding(2)]] Texture2D          g_depth;
[[vk::binding(3)]] SamplerState       g_samp;
[[vk::binding(4)]] RWTexture2D<float4> g_out;

struct Params {
    float4x4 cur_inv_view_proj; // reconstruct world from this frame's depth
    float4x4 prev_view_proj;    // world -> previous frame clip
    float2   extent;
    float    alpha;             // history weight (e.g. 0.9)
    float    valid;             // 0 on the first frame / after a resize
};
[[vk::push_constant]] Params pc;

float3 clip_to_aabb(float3 cmin, float3 cmax, float3 hist) {
    float3 center = 0.5 * (cmin + cmax);
    float3 ext = 0.5 * (cmax - cmin) + 1e-5;
    float3 v = hist - center;
    float3 a = abs(v / ext);
    float ma = max(a.x, max(a.y, a.z));
    return ma > 1.0 ? center + v / ma : hist;
}

[shader("compute")]
[numthreads(8, 8, 1)]
void compute_main(uint3 tid : SV_DispatchThreadID) {
    uint w = 0, h = 0;
    g_out.GetDimensions(w, h);
    if (tid.x >= w || tid.y >= h) return;
    int2 c = int2(tid.xy);
    float2 uv = (float2(c) + 0.5) / float2(w, h);

    float3 cur = g_current.Load(int3(c, 0)).rgb;

    // 3x3 colour neighbourhood AABB of the current frame.
    float3 nmin = cur;
    float3 nmax = cur;
    for (int dy = -1; dy <= 1; ++dy) {
        for (int dx = -1; dx <= 1; ++dx) {
            int2 sc = clamp(c + int2(dx, dy), int2(0, 0), int2(int(w) - 1, int(h) - 1));
            float3 s = g_current.Load(int3(sc, 0)).rgb;
            nmin = min(nmin, s);
            nmax = max(nmax, s);
        }
    }

    // Reproject this pixel into the previous frame.
    float depth = g_depth.Load(int3(c, 0)).r;
    float4 clip = float4(uv * 2.0 - 1.0, depth, 1.0);
    float4 world = mul(pc.cur_inv_view_proj, clip);
    world /= world.w;
    float4 pclip = mul(pc.prev_view_proj, float4(world.xyz, 1.0));
    float2 puv = (pclip.xy / pclip.w) * 0.5 + 0.5;

    float3 result;
    if (pc.valid < 0.5 || any(puv < 0.0) || any(puv > 1.0)) {
        result = cur; // no usable history -> show the current frame
    } else {
        float3 hist = g_history.SampleLevel(g_samp, puv, 0).rgb;
        hist = clip_to_aabb(nmin, nmax, hist);
        result = lerp(cur, hist, pc.alpha);
    }
    g_out[c] = float4(result, 1.0);
}
