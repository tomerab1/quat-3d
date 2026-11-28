// exposure_histogram.slang — log-luminance histogram (R22.2.1; Phase 8.3).
//
// One thread per HDR pixel: undo the frame's pre-exposure, take log2 of the
// luminance and atomically add the pixel's metering weight (fixed point;
// centre-weighted, see metering_weight) into a 256-bin histogram; bins 1..255
// span [min_log2, min_log2 + range] (40 EV: a 10⁻⁴ lux night to a 10⁵ lux
// noon), bin 0 holds black, NaN and anything darker. Workgroup-local
// counts first (one global atomic per bin and group, not per pixel). The
// histogram feeds exposure_average.slang. Mirrors
// engine::renderer::photometry::histogram_bin. (Opsenica 2019 histogram
// metering; Lagarde & de Rousiers 2014 pre-exposure.)

[[vk::binding(0)]] Texture2D                g_hdr;
[[vk::binding(1)]] RWStructuredBuffer<uint> g_hist;

struct Params {
    float min_log_lum;
    float inv_log_lum_range;
    uint  width;
    uint  height;
    float inv_pre_exposure; // HDR values are pre-exposed: true = stored / pre
    float center_weight;    // 0 = average metering, 1 = fully centre-weighted
    float pad0, pad1;
};
[[vk::push_constant]] Params pc;

groupshared uint s_bins[256];

// Fixed-point unit of one pixel's full weight (photometry::k_metering_weight_one).
static const float WEIGHT_ONE = 256.0;

// Centre-weighted metering (a camera's centre-weighted average mode;
// Unreal's metering mask): a Gaussian over the frame's normalised radius r
// (0 centre, 1 corners), sigma 0.4, lifted so the corners still count
// 1 - center_weight. Mirrors photometry::metering_weight.
float metering_weight(uint2 px) {
    float2 uv = (float2(px) + 0.5) / float2(pc.width, pc.height);
    float r2 = dot(uv - 0.5, uv - 0.5) * 2.0; // corners: 1
    float g = exp(-r2 / (2.0 * 0.4 * 0.4));
    return 1.0 - pc.center_weight + pc.center_weight * g;
}

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
        uint w = uint(round(metering_weight(tid.xy) * WEIGHT_ONE));
        InterlockedAdd(s_bins[bin_for(lum)], w);
    }
    GroupMemoryBarrierWithGroupSync();
    if (s_bins[gi] != 0u) InterlockedAdd(g_hist[gi], s_bins[gi]);
}
