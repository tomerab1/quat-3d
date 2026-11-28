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
    float log_inv_pre_exposure; // log2(1 / the frame's pre-exposure)
    float reset;                // > 0.5: seed from this frame's metering (a cut)
    float pad0, pad1;
};

static const float UNDEREXPOSED_STEP_EV = 4.0;
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
    // At least one pixel's weight (histogram_bin weights are fixed point,
    // 256 per pixel), so equal percentiles still pick a pixel.
    const float hi = max(total * pc.high_percent, min(lo + 256.0, total));
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
        // A camera cut or a new scene restarts from the metered value
        // instead of adapting from the previous shot (headless captures
        // converge in one frame).
        const bool seeded = g_exposure[0] > 0.0 && isfinite(g_exposure[2]) && pc.reset < 0.5;
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
        } else {
            // Nothing measurable: the scene is either black or darker than
            // this pre-exposure lets RGBA16F hold (a night loaded with a day
            // exposure). Assume the latter and jump below the darkest
            // luminance the frame could have shown (the smallest normal half,
            // 2^-14, over the pre-exposure), UNDEREXPOSED_STEP_EV further; the
            // EV100 clamp bounds the climb, and a genuinely black frame stays
            // black at any exposure. photometry::k_underexposed_step_ev.
            state = min(seeded ? state : 0.0, -14.0 + pc.log_inv_pre_exposure) -
                    UNDEREXPOSED_STEP_EV;
        }
        // Never adapt far beyond the EV100 range (the exposure clamps there
        // anyway; this keeps the state from drifting off while a frame stays
        // black). The margin covers the adaptive key, which is at most
        // 4.4 EV below the user key.
        state = clamp(state, log2(pc.key / pc.max_exposure) - 6.0,
                      log2(pc.key / pc.min_exposure) + 1.0);
        const float adapted = exp2(state);
        float key = pc.key;
        if (pc.adaptive_key > 0.5) {
            key *= saturate(krawczyk_key(adapted * pc.nits_per_unit) / pc.key_reference);
        }
        // Still unseeded after a black frame: stay so (adapted 0), so the
        // first frame with something measurable seeds directly instead of
        // adapting slowly from the underexposure guess.
        g_exposure[0] = (total > 0.0 || seeded) ? adapted : 0.0;
        g_exposure[1] = clamp(key / adapted, pc.min_exposure, pc.max_exposure);
        g_exposure[2] = state;
        g_exposure[3] = key;
    }
}
