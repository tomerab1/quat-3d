// exposure_histogram.slang — log-luminance histogram (R22.2.1; Phase 8.3).
//
// One thread per HDR pixel: undo the frame's pre-exposure, take log2 of the
// luminance and atomically count it into a 256-bin histogram; bins 1..255
// span [min_log2, min_log2 + range] (40 EV: a 10⁻⁴ lux night to a 10⁵ lux
// noon), bin 0 holds black, NaN and anything darker. Workgroup-local
// counts first (one global atomic per bin and group, not per pixel). The
// histogram feeds exposure_average.slang. Mirrors
// engine::renderer::photometry::histogram_bin. (Tardif / DICE histogram
// metering; Lagarde & de Rousiers 2014 pre-exposure.)

[[vk::binding(0)]] Texture2D                g_hdr;
[[vk::binding(1)]] RWStructuredBuffer<uint> g_hist;

struct Params {
    float min_log_lum;
    float inv_log_lum_range;
    uint  width;
    uint  height;
    float inv_pre_exposure; // HDR values are pre-exposed: true = stored / pre
    float pad0, pad1, pad2;
};
[[vk::push_constant]] Params pc;

groupshared uint s_bins[256];

uint bin_for(float lum) {
    // Black (and NaN, which fails every comparison) -> reserved bin 0.
    if (!(lum >= exp2(pc.min_log_lum))) return 0;
    float t = saturate((log2(lum) - pc.min_log_lum) * pc.inv_log_lum_range);
    return 1u + min(uint(t * 255.0), 254u); // bins 1..255
}

[shader("compute")]
[numthreads(16, 16, 1)]
void compute_main(uint3 tid : SV_DispatchThreadID, uint gi : SV_GroupIndex) {
    s_bins[gi] = 0u;
    GroupMemoryBarrierWithGroupSync();
    if (tid.x < pc.width && tid.y < pc.height) {
        float3 c = g_hdr.Load(int3(int2(tid.xy), 0)).rgb;
        float lum = dot(c, float3(0.2126, 0.7152, 0.0722)) * pc.inv_pre_exposure;
        InterlockedAdd(s_bins[bin_for(lum)], 1u);
    }
    GroupMemoryBarrierWithGroupSync();
    if (s_bins[gi] != 0u) InterlockedAdd(g_hist[gi], s_bins[gi]);
}
