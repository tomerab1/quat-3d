// exposure_average.slang — histogram metering and eye adaptation (R22.2.1).
//
// One 256-thread workgroup. An inclusive prefix sum over the bins gives each
// bin's place in the brightness order; the metered luminance is the mean
// log2 luminance of the pixels between the low and high percentiles (the
// standard histogram metering: specular glints and black sky corners do not
// swing the exposure). It is smoothed over time in the log domain
// (exponential, time constant tau), then mapped to an exposure by the
// adaptive key of Krawczyk, Myszkowski & Seidel 2005 and clamped to the
// configured EV100 range (Lagarde & de Rousiers 2014). CPU mirror:
// engine::renderer::photometry (metered_log2, adapt_log2, auto_exposure).
//
// g_exposure (host-visible, persists across frames):
//   [0] adapted luminance (HDR units, not pre-exposed; 0 = not yet seeded)
//   [1] auto exposure (HDR units, before compensation)
//   [2] adapted log2 luminance (the smoothed state)
//   [3] key the adapted luminance maps to

[[vk::binding(0)]] RWStructuredBuffer<uint>  g_hist;
[[vk::binding(1)]] RWStructuredBuffer<float> g_exposure;

struct Params {
    float min_log_lum;
    float log_lum_range;
    float dt;
    float tau;           // adaptation time constant (seconds)
    float low_percent;
    float high_percent;
    float key;           // user key (mid-grey)
    float adaptive_key;  // > 0.5: Krawczyk's key
    float min_exposure;  // EV100 clamps as exposures (HDR units)
    float max_exposure;
    float nits_per_unit;
    float key_reference; // Krawczyk key at the reference luminance
};
[[vk::push_constant]] Params pc;

groupshared float s_scan[256];

float krawczyk_key(float nits) {
    return 1.03 - 2.0 / (2.0 + log10(max(nits, 0.0) + 1.0));
}

[shader("compute")]
[numthreads(256, 1, 1)]
void compute_main(uint gi : SV_GroupIndex) {
    const float count = gi == 0u ? 0.0 : float(g_hist[gi]); // bin 0: black, excluded
    s_scan[gi] = count;
    GroupMemoryBarrierWithGroupSync();
    // Hillis-Steele inclusive scan.
    for (uint offset = 1u; offset < 256u; offset <<= 1) {
        const float add = gi >= offset ? s_scan[gi - offset] : 0.0;
        GroupMemoryBarrierWithGroupSync();
        s_scan[gi] += add;
        GroupMemoryBarrierWithGroupSync();
    }
    const float total = s_scan[255];
    const float below = s_scan[gi] - count;
    const float lo = total * pc.low_percent;
    const float hi = max(total * pc.high_percent, min(lo + 1.0, total));
    const float overlap = max(min(below + count, hi) - max(below, lo), 0.0);
    const float bin_log = pc.min_log_lum + (float(gi) - 0.5) * (pc.log_lum_range / 255.0);
    GroupMemoryBarrierWithGroupSync();

    // Reduce (overlap, overlap * log) — two passes over the same array.
    s_scan[gi] = overlap;
    GroupMemoryBarrierWithGroupSync();
    for (uint cutoff = 128u; cutoff > 0u; cutoff >>= 1) {
        if (gi < cutoff) s_scan[gi] += s_scan[gi + cutoff];
        GroupMemoryBarrierWithGroupSync();
    }
    const float weight = s_scan[0];
    GroupMemoryBarrierWithGroupSync();
    s_scan[gi] = overlap * bin_log;
    GroupMemoryBarrierWithGroupSync();
    for (uint cutoff = 128u; cutoff > 0u; cutoff >>= 1) {
        if (gi < cutoff) s_scan[gi] += s_scan[gi + cutoff];
        GroupMemoryBarrierWithGroupSync();
    }

    if (gi == 0u) {
        const bool seeded = g_exposure[0] > 0.0 && isfinite(g_exposure[2]);
        float state = g_exposure[2];
        if (total > 0.0) {
            const float target = s_scan[0] / max(weight, 1e-6);
            // First frame (or a non-finite state, never expected but
            // persistent) seeds directly.
            if (!seeded) {
                state = target;
            } else {
                const float rate = 1.0 - exp(-pc.dt / max(pc.tau, 1e-3));
                state += (target - state) * rate;
            }
        } else if (!seeded) {
            state = log2(0.18); // all black and never seeded: a neutral start
        }
        const float adapted = exp2(state);
        float key = pc.key;
        if (pc.adaptive_key > 0.5) {
            key *= saturate(krawczyk_key(adapted * pc.nits_per_unit) / pc.key_reference);
        }
        g_exposure[0] = adapted;
        g_exposure[1] = clamp(key / adapted, pc.min_exposure, pc.max_exposure);
        g_exposure[2] = state;
        g_exposure[3] = key;
    }
}
