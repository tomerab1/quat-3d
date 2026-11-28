// taa.slang — temporal anti-aliasing and upsampling resolve (TAAU; R11, after
// Phase 8.1 and the S1 A2/A14 fixes).
//
// One thread per DISPLAY pixel. Render samples (render extent, jittered,
// linear HDR) sit at (i + 0.5 - jitter) / render in unjittered UV
// (jitter.hpp). The current frame is reconstructed at the pixel from its 3x3
// nearest samples with a Gaussian in display pixels, and its confidence is
// the kernel weight of the nearest sample: high when a sample lands on the
// pixel, low when the nearest one is a display pixel or more away (reduced
// render resolution).
//
// The history (display extent) is reprojected with the velocity target
// (velocity.hpp) at the closest-depth sample of the 3x3 (depth dilation:
// edges of foreground objects carry their own motion), sampled with a 5-tap
// Catmull-Rom filter and clipped toward the current neighbourhood (variance
// clipping, Salvi 2016, inside the min/max box). Its alpha channel holds the
// accumulated confidence: each frame adds the current confidence (capped),
// and the current frame is blended in with weight confidence / accumulated,
// a running average over the samples that landed on the pixel — the same
// sample-weight accumulation temporal upscalers (FSR 2, TSR) use. Clipping
// that moves the history scales its accumulated weight down, so rejected
// history is replaced quickly. Neighbourhood statistics, clipping and
// blending happen in exposed, Reinhard-compressed YCoCg (Karis 2014), which
// keeps bright HDR samples from dominating; colours are stored linear.
//
// Two rgba16f storage outputs with explicit formats: the next frame's history
// and the displayed result (post-processing may modify it). The running
// average blends far-away samples in with weights well below 1/1000, which
// plain half-float rounding drops (the history stalls and drifts); the
// history write therefore rounds stochastically onto the half-float grid
// (per-pixel, per-frame dither), so every update survives on average. The
// rounding is explicit: the storage conversion's own mode is
// implementation-defined (lavapipe truncates, which drifted the history dark).
//
// NaN / Inf guards (issue #45): a non-finite colour sample, velocity
// (reprojection) or history value never enters the persistent history — the
// sample counts as black, the history is dropped for the pixel — and each one
// is counted into NonFiniteCounters when the debug counter is on. Both
// outputs go through hdr_store (lib/hdr.slang), so the resolve never writes
// an Inf into its half-float targets.

import hdr; // hdr_store

Texture2D               g_color;    // render extent, linear HDR
Texture2D               g_depth;    // render extent, [0, 1], 1 = far
Texture2D<float2>       g_velocity; // render extent, uv_current - uv_previous
Texture2D               g_history;  // display extent, rgb last result, a accumulated weight
SamplerState            g_linear;   // bilinear, clamp to edge
StructuredBuffer<float> g_exposure; // ExposureState: [1] = auto exposure
[format("rgba16f")] RWTexture2D<float4> g_history_out;
[format("rgba16f")] RWTexture2D<float4> g_out;
// Debug counter (NonFiniteCounters), bound as a descriptor: an atomic through
// a buffer device address is mistranslated by
// MoltenVK's SPIRV-Cross into Metal that does not compile.
// Always bound (a dummy when counting is off); written only under the flag.
RWStructuredBuffer<uint> g_non_finite;

// Mirrors TaaPushConstants in taa_pass.cpp (96 bytes).
struct Params {
    float4 render_size;  // xy extent, zw 1 / extent
    float4 display_size; // xy extent, zw 1 / extent
    float4 jitter;       // xy jitter (render pixels), z sharpness, w history valid
    float4 exposure;     // x read g_exposure, y 1 / pre-exposure, z history rescale
                         // (this frame's pre-exposure / the history's)
    float4 blend;        // x max accumulated weight, y sample falloff (per display px^2),
                         // z clip rejection (weight lost per unit of clipping),
                         // w frame index (dither seed)
    uint   count_non_finite; // 1: count rejected values into g_non_finite[0]
    uint   pad0;
    uint   pad1;
    uint   pad2;
}
[[vk::push_constant]] Params pc;

static const float CLIP_GAMMA = 1.25;

// Interleaved gradient noise (Jimenez 2014), shifted by the golden ratio
// each frame: a well-distributed uniform in [0, 1) per pixel and frame.
float dither_noise(uint2 pixel, float frame) {
    const float ign = frac(52.9829189 * frac(dot(float2(pixel), float2(0.06711056, 0.00583715))));
    return frac(ign + 0.618034 * frame);
}

// Rounds non-negative values onto the half-float grid stochastically: down or
// up to the neighbouring representable value with probabilities that make the
// result unbiased (u uniform in [0, 1)). The result is exactly representable,
// so the storage conversion's own rounding mode (implementation-defined)
// cannot reintroduce a bias.
float4 stochastic_round_half(float4 v, float4 u) {
    v = max(v, 0.0);
    // Binade exponent from the float bits; half floats go subnormal below 2^-14.
    const int4 e = max(int4((asuint(v) >> 23u) & 0xFFu) - 127, int4(-14));
    const float4 step = ldexp(float4(1.0), float4(e - 10)); // 10 mantissa bits
    return floor(v / step + u) * step;
}

void count_non_finite() {
    if (pc.count_non_finite != 0) {
        uint previous;
        InterlockedAdd(g_non_finite[0], 1u, previous);
    }
}

float3 finite_or_zero(float3 c) {
    if (all(isfinite(c))) return c;
    count_non_finite();
    return float3(0.0, 0.0, 0.0);
}

float max3(float3 c) {
    return max(c.x, max(c.y, c.z));
}

float3 rgb_to_ycocg(float3 c) {
    return float3(0.25 * c.r + 0.5 * c.g + 0.25 * c.b, 0.5 * c.r - 0.5 * c.b,
                  -0.25 * c.r + 0.5 * c.g - 0.25 * c.b);
}

float3 ycocg_to_rgb(float3 c) {
    const float t = c.x - c.z;
    return float3(t + c.y, c.x + c.z, t - c.y);
}

// Linear HDR -> the resolve space (exposed, c / (1 + max(c)), YCoCg) and back.
float3 to_resolve(float3 linear, float exposure) {
    const float3 c = max(linear, 0.0) * exposure;
    return rgb_to_ycocg(c / (1.0 + max3(c)));
}

float3 from_resolve(float3 r, float exposure) {
    const float3 c = max(ycocg_to_rgb(r), 0.0);
    return c / max(1.0 - max3(c), 1e-4) / exposure;
}

float3 clip_to_aabb(float3 cmin, float3 cmax, float3 hist) {
    const float3 center = 0.5 * (cmin + cmax);
    const float3 ext = 0.5 * (cmax - cmin) + 1e-6;
    const float3 v = hist - center;
    const float3 a = abs(v / ext);
    const float ma = max(a.x, max(a.y, a.z));
    return ma > 1.0 ? center + v / ma : hist;
}

// Catmull-Rom history sample from five bilinear taps (corners dropped).
// Exact at texel centres, so a static history is never blurred.
float4 sample_history(float2 uv) {
    const float2 pos = uv * pc.display_size.xy;
    const float2 c1 = floor(pos - 0.5) + 0.5;
    const float2 f = pos - c1;
    const float2 w0 = f * (-0.5 + f * (1.0 - 0.5 * f));
    const float2 w1 = 1.0 + f * f * (-2.5 + 1.5 * f);
    const float2 w2 = f * (0.5 + f * (2.0 - 1.5 * f));
    const float2 w3 = f * f * (-0.5 + 0.5 * f);
    const float2 w12 = w1 + w2;
    const float2 t0 = (c1 - 1.0) * pc.display_size.zw;
    const float2 t3 = (c1 + 2.0) * pc.display_size.zw;
    const float2 t12 = (c1 + w2 / w12) * pc.display_size.zw;
    const float wa = w12.x * w0.y;
    const float wb = w0.x * w12.y;
    const float wc = w12.x * w12.y;
    const float wd = w3.x * w12.y;
    const float we = w12.x * w3.y;
    const float4 sum = g_history.SampleLevel(g_linear, float2(t12.x, t0.y), 0) * wa +
                       g_history.SampleLevel(g_linear, float2(t0.x, t12.y), 0) * wb +
                       g_history.SampleLevel(g_linear, t12, 0) * wc +
                       g_history.SampleLevel(g_linear, float2(t3.x, t12.y), 0) * wd +
                       g_history.SampleLevel(g_linear, float2(t12.x, t3.y), 0) * we;
    // Raw (Catmull-Rom can undershoot below 0): the caller tests the result
    // for NaN / Inf before clamping it, since max() may hide a NaN.
    return sum / (wa + wb + wc + wd + we);
}

[shader("compute")]
[numthreads(8, 8, 1)]
void compute_main(uint3 tid : SV_DispatchThreadID) {
    if (tid.x >= uint(pc.display_size.x) || tid.y >= uint(pc.display_size.y)) return;
    const int2 p = int2(tid.xy);
    const float2 uv = (float2(p) + 0.5) * pc.display_size.zw;
    const int2 render_max = int2(pc.render_size.xy) - 1;
    const float ratio = pc.display_size.x * pc.render_size.z; // display / render per axis
    const float falloff = pc.blend.y * ratio * ratio;          // per render px^2

    // Exposed space: the colour is pre-exposed (R22.2.1), so the exposure
    // relative to it is the auto exposure over the pre-exposure (~1; the
    // previous frame's value). Clamped so a stale value cannot flatten the
    // resolve.
    float exposure = 1.0;
    if (pc.exposure.x > 0.5) {
        exposure = clamp(g_exposure[1] * pc.exposure.y, 1.0 / 64.0, 64.0);
        if (!isfinite(exposure)) exposure = 1.0;
    }

    // This pixel in render-sample coordinates: sample i sits at rpos == i.
    const float2 rpos = uv * pc.render_size.xy - 0.5 + pc.jitter.xy;
    const int2 centre = int2(floor(rpos + 0.5));

    float3 sum = 0.0;
    float  weight = 0.0;
    float3 m1 = 0.0;
    float3 m2 = 0.0;
    float3 nmin = 1e9;
    float3 nmax = -1e9;
    const float2 to_nearest = float2(centre) - rpos;
    const float nearest_d2 = dot(to_nearest, to_nearest);
    float  closest_depth = 2.0;
    int2   closest = clamp(centre, int2(0, 0), render_max);
    for (int dy = -1; dy <= 1; ++dy) {
        for (int dx = -1; dx <= 1; ++dx) {
            const int2 q = clamp(centre + int2(dx, dy), int2(0, 0), render_max);
            // Non-finite input never enters the persistent history (it would
            // stay there and spread through the neighbourhood clip every frame).
            const float3 c = to_resolve(finite_or_zero(g_color.Load(int3(q, 0)).rgb), exposure);
            const float2 d = float2(q) - rpos;
            // Relative to the nearest sample (normalised below), so a kernel
            // far from every sample never underflows to all zeros.
            const float w = exp(-falloff * max(dot(d, d) - nearest_d2, 0.0));
            sum += c * w;
            weight += w;
            m1 += c;
            m2 += c * c;
            nmin = min(nmin, c);
            nmax = max(nmax, c);
            const float depth = g_depth.Load(int3(q, 0)).r;
            if (depth < closest_depth) {
                closest_depth = depth;
                closest = q;
            }
        }
    }
    const float3 mean = m1 / 9.0;
    float3 current = sum / weight;
    current = clamp(current + pc.jitter.z * (current - mean), nmin, nmax);
    const float confidence = exp(-falloff * nearest_d2);

    float3 result = current;
    float  accumulated = confidence;
    const float2 velocity = g_velocity.Load(int3(closest, 0));
    const float2 prev_uv = uv - velocity;
    // A non-finite reprojection has no usable history: NaN fails every
    // comparison below, so test it first (and count it).
    const bool reprojected = all(isfinite(velocity));
    if (pc.jitter.w > 0.5 && !reprojected) count_non_finite();
    if (pc.jitter.w > 0.5 && reprojected && all(prev_uv >= 0.0) && all(prev_uv <= 1.0)) {
        const float4 raw_history = sample_history(prev_uv);
        if (all(isfinite(raw_history))) {
            // Re-expose the history if the pre-exposure moved since it was stored.
            const float4 h = max(raw_history, 0.0) * float4(pc.exposure.zzz, 1.0);
            const float3 sigma = sqrt(max(m2 / 9.0 - mean * mean, 0.0));
            const float3 box_min = max(nmin, mean - CLIP_GAMMA * sigma);
            const float3 box_max = min(nmax, mean + CLIP_GAMMA * sigma);
            const float3 raw = to_resolve(h.rgb, exposure);
            const float3 history = clip_to_aabb(box_min, box_max, raw);
            const float kept = saturate(1.0 - pc.blend.z * length(history - raw));
            accumulated = min(h.a * kept + confidence, pc.blend.x);
            result = lerp(history, current, confidence / max(accumulated, 1e-6));
        } else {
            count_non_finite(); // dropped: the pixel restarts from this frame
        }
    }

    const float3 linear = hdr_store(from_resolve(result, exposure));
    const float u = dither_noise(tid.xy, pc.blend.w);
    g_history_out[p] = stochastic_round_half(float4(linear, accumulated), float4(u));
    g_out[p] = float4(linear, 1.0);
}
