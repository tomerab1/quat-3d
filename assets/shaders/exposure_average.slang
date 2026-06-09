// exposure_average.slang — average luminance + temporal adaptation (Phase 8.3).
//
// A single 256-thread workgroup reduces the histogram into a weighted average
// bin, maps it back to luminance (excluding the black bin), then eases the
// stored adapted luminance toward it over time (exponential, time-constant tau).
// The tonemap pass reads the adapted luminance to drive exposure.

[[vk::binding(0)]] RWStructuredBuffer<uint>  g_hist;
[[vk::binding(1)]] RWStructuredBuffer<float> g_exposure; // [0] = adapted avg luminance

struct Params {
    float min_log_lum;
    float log_lum_range;
    float dt;
    float num_pixels;
    float tau; // adaptation time constant (seconds)
    float pad0, pad1, pad2;
};
[[vk::push_constant]] Params pc;

groupshared uint s_weighted[256];

[shader("compute")]
[numthreads(256, 1, 1)]
void compute_main(uint gi : SV_GroupIndex) {
    uint count = g_hist[gi];
    s_weighted[gi] = count * gi; // bin index weighted by population
    GroupMemoryBarrierWithGroupSync();

    for (uint cutoff = 128u; cutoff > 0u; cutoff >>= 1) {
        if (gi < cutoff) s_weighted[gi] += s_weighted[gi + cutoff];
        GroupMemoryBarrierWithGroupSync();
    }

    if (gi == 0) {
        uint black = g_hist[0];
        float denom = max(pc.num_pixels - float(black), 1.0);
        float avg_bin = float(s_weighted[0]) / denom; // mean bin over the lit pixels
        float avg_log = (avg_bin - 1.0) / 254.0 * pc.log_lum_range + pc.min_log_lum;
        float avg_lum = exp2(avg_log);

        float prev = g_exposure[0];
        if (prev <= 0.0) prev = avg_lum; // first frame seeds directly
        float rate = 1.0 - exp(-pc.dt / max(pc.tau, 1e-3));
        g_exposure[0] = prev + (avg_lum - prev) * rate;
    }
}
