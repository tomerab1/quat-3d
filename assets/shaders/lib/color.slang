// color.slang — the final image's colour pipeline (ROADMAP R20): tonemap
// operators behind one interface, scene-referred grading in ACEScct and the
// look-LUT lookup. The CPU references are in engine/renderer/color_grading.hpp
// (src/renderer/color_grading.cpp); keep the two in step.
//
// Matrices: Slang's float3x3(a, b, c) takes ROWS and mul(M, v) is M * v.
// The AgX matrices are published as GLSL column vectors; they are written
// here as rows of the transpose and applied as mul(v, M), which is the same
// product.

// ---- Tonemap operators -----------------------------------------------------

// Linear Rec.709 radiance (exposed) -> display-linear [0, 1].
public interface ITonemapper {
    public float3 map(float3 x);
}

// Narkowicz's ACES fit (the engine's original operator).
public struct AcesTonemapper : ITonemapper {
    public float3 map(float3 x) {
        const float a = 2.51;
        const float b = 0.03;
        const float c = 2.43;
        const float d = 0.59;
        const float e = 0.14;
        return saturate((x * (a * x + b)) / (x * (c * x + d) + e));
    }
}

// AgX as Blender 4 ships it (Sobotka; the Filament / three.js port with the
// outset matrix and Wrensch's sigmoid fit).
public struct AgxTonemapper : ITonemapper {
    static float3 contrast(float3 x) {
        const float3 x2 = x * x;
        const float3 x4 = x2 * x2;
        return 15.5 * x4 * x2 - 40.14 * x4 * x + 31.96 * x4 - 6.868 * x2 * x + 0.4298 * x2 +
               0.1191 * x - 0.00232;
    }

    public float3 map(float3 x) {
        const float3x3 rec709_to_rec2020 = float3x3(
            float3(0.6274, 0.0691, 0.0164), float3(0.3293, 0.9195, 0.0880),
            float3(0.0433, 0.0113, 0.8956));
        const float3x3 rec2020_to_rec709 = float3x3(
            float3(1.6605, -0.1246, -0.0182), float3(-0.5876, 1.1329, -0.1006),
            float3(-0.0728, -0.0083, 1.1187));
        const float3x3 inset = float3x3(
            float3(0.856627153315983, 0.137318972929847, 0.11189821299995),
            float3(0.0951212405381588, 0.761241990602591, 0.0767994186031903),
            float3(0.0482516061458583, 0.101439036467562, 0.811302368396859));
        const float3x3 outset = float3x3(
            float3(1.1271005818144368, -0.1413297634984383, -0.14132976349843826),
            float3(-0.11060664309660323, 1.157823702216272, -0.11060664309660294),
            float3(-0.016493938717834573, -0.016493938717834257, 1.2519364065950405));
        const float min_ev = -12.47393;
        const float max_ev = 4.026069;
        float3 c = mul(mul(x, rec709_to_rec2020), inset);
        c = saturate((log2(max(c, 1e-10)) - min_ev) / (max_ev - min_ev));
        c = mul(contrast(c), outset);
        c = pow(max(c, 0.0), 2.2);
        return saturate(mul(c, rec2020_to_rec709));
    }
}

// Khronos PBR Neutral (2024).
public struct PbrNeutralTonemapper : ITonemapper {
    public float3 map(float3 color) {
        const float start_compression = 0.8 - 0.04;
        const float desaturation = 0.15;
        const float x = min(color.r, min(color.g, color.b));
        const float offset = x < 0.08 ? x - 6.25 * x * x : 0.04;
        color -= offset;
        const float peak = max(color.r, max(color.g, color.b));
        if (peak < start_compression) return color;
        const float d = 1.0 - start_compression;
        const float new_peak = 1.0 - d * d / (peak + d - start_compression);
        color *= new_peak / peak;
        const float g = 1.0 - 1.0 / (desaturation * (peak - new_peak) + 1.0);
        return lerp(color, float3(new_peak), g);
    }
}

// ---- Transfer functions ------------------------------------------------------

public float3 linear_to_srgb(float3 c) {
    const float3 lo = c * 12.92;
    const float3 hi = 1.055 * pow(max(c, 0.0), 1.0 / 2.4) - 0.055;
    return select(c <= 0.0031308, lo, hi);
}

public float acescct_from_linear(float x) {
    return x <= 0.0078125 ? 10.5402377416545 * x + 0.0729055341958355 : (log2(x) + 9.72) / 17.52;
}

public float linear_from_acescct(float x) {
    return x <= 0.155251141552511 ? (x - 0.0729055341958355) / 10.5402377416545
                                  : min(exp2(x * 17.52 - 9.72), 65504.0);
}

// ---- Scene-referred grading --------------------------------------------------

// Mirrors engine::renderer::grading::GradingGpu (96 bytes).
public struct Grading {
    public float4 white_balance[3]; // rows of the linear Rec.709 matrix
    public float4 lift;             // rgb lift, w contrast
    public float4 inv_gamma;        // rgb 1 / gamma, w saturation
    public float4 gain;             // rgb gain, w 1 = grade
}

static const float ACESCCT_MID_GREY = 0.4135884;

float pow_signed(float x, float p) {
    return x < 0.0 ? -pow(-x, p) : pow(x, p);
}

// White balance, then contrast / lift / gamma / gain in ACEScct, then
// saturation in linear AP1 (grading::grade on the CPU).
public float3 grade(Grading g, float3 c) {
    if (g.gain.w < 0.5) return c;
    const float3x3 rec709_to_ap1 = float3x3(
        float3(0.6130974024, 0.3395231462, 0.0473794514),
        float3(0.0701937225, 0.9163538791, 0.0134523985),
        float3(0.0206155929, 0.1095697729, 0.8698146342));
    const float3x3 ap1_to_rec709 = float3x3(
        float3(1.7050509927, -0.6217921207, -0.0832588720),
        float3(-0.1302564176, 1.1408047365, -0.0105483191),
        float3(-0.0240033568, -0.1289689760, 1.1529723328));
    const float3x3 wb = float3x3(g.white_balance[0].xyz, g.white_balance[1].xyz,
                                 g.white_balance[2].xyz);
    float3 ap1 = mul(rec709_to_ap1, mul(wb, c));
    [unroll]
    for (int i = 0; i < 3; ++i) {
        float v = acescct_from_linear(ap1[i]);
        v = (v - ACESCCT_MID_GREY) * g.lift.w + ACESCCT_MID_GREY;
        v = g.gain[i] * (v + g.lift[i] * (1.0 - v));
        v = pow_signed(v, g.inv_gamma[i]);
        ap1[i] = linear_from_acescct(v);
    }
    const float luma = dot(ap1, float3(0.2722287, 0.6740818, 0.0536895));
    ap1 = luma + (ap1 - luma) * g.inv_gamma.w;
    return max(mul(ap1_to_rec709, ap1), 0.0);
}

// ---- Look LUT ----------------------------------------------------------------

// A size^3 LUT as a 3D texture (x red, y green, z blue): one hardware
// trilinear lookup at the texel centres that span [0, 1], so the end points
// map exactly to the table's first and last entries. `c` is in [0, 1].
public float3 sample_lut(Texture3D lut, SamplerState linear_clamp, float3 c, float size) {
    const float3 uvw = (saturate(c) * (size - 1.0) + 0.5) / size;
    return lut.SampleLevel(linear_clamp, uvw, 0.0).rgb;
}
