// exposure_histogram.slang — luminance histogram build (Phase 8.3).
//
// One thread per HDR pixel: compute log-luminance, map it into a 256-bin
// histogram (bin 0 reserved for near-black), and atomically increment the bin.
// The histogram feeds exposure_average.slang. (Tardif / DICE auto-exposure.)

[[vk::binding(0)]] Texture2D                g_hdr;
[[vk::binding(1)]] RWStructuredBuffer<uint> g_hist;

struct Params {
    float min_log_lum;
    float inv_log_lum_range;
    uint  width;
    uint  height;
};
[[vk::push_constant]] Params pc;

uint bin_for(float lum) {
    if (lum < 1e-5) return 0; // black -> reserved bin 0
    float t = saturate((log2(lum) - pc.min_log_lum) * pc.inv_log_lum_range);
    return uint(t * 254.0 + 1.0); // bins 1..255
}

[shader("compute")]
[numthreads(16, 16, 1)]
void compute_main(uint3 tid : SV_DispatchThreadID) {
    if (tid.x >= pc.width || tid.y >= pc.height) return;
    float3 c = g_hdr.Load(int3(int2(tid.xy), 0)).rgb;
    float lum = dot(c, float3(0.2126, 0.7152, 0.0722));
    uint bin = bin_for(lum);
    InterlockedAdd(g_hist[bin], 1u);
}
