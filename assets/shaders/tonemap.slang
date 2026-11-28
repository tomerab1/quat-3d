// tonemap.slang — the final image (Phase 3.7; R20 cinematic post).
//
// A fullscreen triangle reads the linear HDR colour (after TAAU, depth of
// field, motion blur and bloom) and produces the display-encoded LDR image:
//
//   lateral chromatic aberration (spectral taps) -> exposure (auto, manual or
//   the physical camera's EV100; computed on the CPU) -> vignette (the lens's
//   cos^4 falloff and an artistic one, applied to light) -> scene-referred
//   grading (lib/color.slang) -> tonemap operator -> sRGB encode -> look LUT
//   -> film grain.
//
// The operator is a Slang interface (lib/color.slang ITonemapper); one
// fragment entry point per implementation, and the pass builds a pipeline
// for each (no #ifdef variants). The output is a plain UNORM target, so the
// sRGB encode is explicit.

import color;

struct VsOut {
    float4 clip : SV_Position;
    float2 uv   : TEXCOORD0;
}

// Fullscreen triangle from the vertex id — no vertex buffer. uv spans [0,2] so
// the visible [-1,1] clip region maps to [0,1] texture space (Vulkan top-left).
[shader("vertex")]
VsOut vertex_main(uint id : SV_VertexID) {
    VsOut o;
    float2 uv = float2(float((id << 1) & 2), float(id & 2));
    o.uv = uv;
    o.clip = float4(uv * 2.0 - 1.0, 0.0, 1.0);
    return o;
}

// Mirrors PostParams in tonemap_pass.cpp (240 bytes).
struct PostParams {
    float4  exposure;     // x 1 = read g_exposure, y key, z min, w max
    float4  exposure2;    // x scale (2^compensation, x physical EV100 exposure)
    Grading grading;
    float4  vignette;     // x natural (cos^4) strength, y artistic, z smoothness, w aspect
    float4  lens;         // xy tan(fov / 2) horizontal / vertical, z chromatic aberration
    float4  grain;        // x intensity, y size (px), z seed (24 Hz grain clock)
    float4  lut;          // x contribution (0 = off), y size
    float4  lut_domain;   // xyz domain min
    float4  lut_scale;    // xyz 1 / (domain max - domain min)
    float4  extent;       // xy output extent, zw 1 / extent
};

Texture2D               g_hdr;
SamplerState            g_sampler;  // bilinear, clamp to edge
StructuredBuffer<float> g_exposure; // [0] = adapted average luminance
ConstantBuffer<PostParams> g_post;
Texture3D               g_lut;      // look LUT (lib/color.slang sample_lut)

// Lateral chromatic aberration: the image is magnified slightly differently
// per wavelength. Five taps along the radial direction, each weighted by a
// red -> green -> blue ramp (normalised per channel), so the fringes are
// smooth spectra rather than three offset copies.
float3 sample_hdr(float2 uv) {
    const float ca = g_post.lens.z;
    if (ca <= 0.0) return g_hdr.SampleLevel(g_sampler, uv, 0.0).rgb;
    const float2 d = uv - 0.5;
    float3 sum = 0.0;
    float3 weight = 0.0;
    [unroll]
    for (int k = 0; k < 5; ++k) {
        const float t = float(k) * 0.25;
        const float3 w = float3(saturate(1.0 - 2.0 * t), 1.0 - abs(2.0 * t - 1.0),
                                saturate(2.0 * t - 1.0));
        // Up to 1% of the half-diagonal of magnification spread at ca = 1.
        const float scale = 1.0 + (0.5 - t) * 0.02 * ca;
        sum += w * g_hdr.SampleLevel(g_sampler, 0.5 + d * scale, 0.0).rgb;
        weight += w;
    }
    return sum / weight;
}

// Illumination falloff: the lens's cos^4 law over the field of view, blended
// by the natural strength, times an artistic radial darkening.
float vignette(float2 uv) {
    const float2 ndc = uv * 2.0 - 1.0;
    const float2 t = ndc * g_post.lens.xy; // tan of the ray angle per axis
    const float cos2 = 1.0 / (1.0 + dot(t, t));
    const float natural = lerp(1.0, cos2 * cos2, g_post.vignette.x);
    // Artistic: 0 at the centre, 1 at the corners (aspect-corrected).
    const float2 p = ndc * float2(g_post.vignette.w, 1.0);
    const float r = length(p) / length(float2(g_post.vignette.w, 1.0));
    const float artistic = 1.0 - g_post.vignette.y * smoothstep(1.0 - g_post.vignette.z, 1.0, r);
    return natural * artistic;
}

float hash12(float2 p) {
    float3 p3 = frac(float3(p.xyx) * 0.1031);
    p3 += dot(p3, p3.yzx + 33.33);
    return frac((p3.x + p3.y) * p3.z);
}

// Smooth value noise in [-1, 1] at the grain size; the pattern changes on a
// fixed 24 Hz clock (not per rendered frame), so it reads as film grain at
// any frame rate instead of sizzling.
float grain_noise(float2 pixel) {
    const float2 p = pixel / g_post.grain.y + g_post.grain.z * float2(17.13, 31.71);
    const float2 i = floor(p);
    const float2 f = p - i;
    const float2 s = f * f * (3.0 - 2.0 * f);
    const float a = hash12(i);
    const float b = hash12(i + float2(1.0, 0.0));
    const float c = hash12(i + float2(0.0, 1.0));
    const float d = hash12(i + float2(1.0, 1.0));
    return lerp(lerp(a, b, s.x), lerp(c, d, s.x), s.y) * 2.0 - 1.0;
}

float4 post_main<T : ITonemapper>(VsOut input, T op) {
    const float2 uv = input.uv;
    float3 hdr = sample_hdr(uv);
    float exposure = g_post.exposure2.x;
    if (g_post.exposure.x > 0.5) {
        const float avg = max(g_exposure[0], 1e-4);
        exposure *= clamp(g_post.exposure.y / avg, g_post.exposure.z, g_post.exposure.w);
    }
    hdr *= exposure * vignette(uv);
    hdr = grade(g_post.grading, hdr);
    float3 ldr = linear_to_srgb(op.map(hdr));
    if (g_post.lut.x > 0.0) {
        const float3 coord = (ldr - g_post.lut_domain.xyz) * g_post.lut_scale.xyz;
        const float3 looked = sample_lut(g_lut, g_sampler, coord, g_post.lut.y);
        ldr = lerp(ldr, looked, g_post.lut.x);
    }
    if (g_post.grain.x > 0.0) {
        // Display-referred, strongest in the mid-tones like film.
        const float luma = dot(saturate(ldr), float3(0.2126, 0.7152, 0.0722));
        const float response = 4.0 * luma * (1.0 - luma);
        ldr += grain_noise(input.clip.xy) * g_post.grain.x * 0.1 * response;
    }
    return float4(saturate(ldr), 1.0);
}

[shader("fragment")]
float4 fragment_aces(VsOut input) : SV_Target {
    return post_main(input, AcesTonemapper());
}

[shader("fragment")]
float4 fragment_agx(VsOut input) : SV_Target {
    return post_main(input, AgxTonemapper());
}

[shader("fragment")]
float4 fragment_pbr_neutral(VsOut input) : SV_Target {
    return post_main(input, PbrNeutralTonemapper());
}
