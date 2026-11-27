#include "engine/renderer/color_grading.hpp"

#include <algorithm>
#include <cmath>

namespace engine::renderer::grading {

namespace {

// glm matrices are column-major; these helpers take the row-major numbers
// as published.
[[nodiscard]] glm::mat3 rows(float a, float b, float c, float d, float e, float f, float g,
                             float h, float i) {
    return glm::transpose(glm::mat3(a, b, c, d, e, f, g, h, i));
}

// AgX (Filament / three.js port of Blender 4's AgX), as GLSL column vectors.
const glm::mat3 k_rec709_to_rec2020 = glm::mat3(glm::vec3(0.6274F, 0.0691F, 0.0164F),
                                                glm::vec3(0.3293F, 0.9195F, 0.0880F),
                                                glm::vec3(0.0433F, 0.0113F, 0.8956F));
const glm::mat3 k_rec2020_to_rec709 = glm::mat3(glm::vec3(1.6605F, -0.1246F, -0.0182F),
                                                glm::vec3(-0.5876F, 1.1329F, -0.1006F),
                                                glm::vec3(-0.0728F, -0.0083F, 1.1187F));
const glm::mat3 k_agx_inset = glm::mat3(
    glm::vec3(0.856627153315983F, 0.137318972929847F, 0.11189821299995F),
    glm::vec3(0.0951212405381588F, 0.761241990602591F, 0.0767994186031903F),
    glm::vec3(0.0482516061458583F, 0.101439036467562F, 0.811302368396859F));
const glm::mat3 k_agx_outset = glm::mat3(
    glm::vec3(1.1271005818144368F, -0.1413297634984383F, -0.14132976349843826F),
    glm::vec3(-0.11060664309660323F, 1.157823702216272F, -0.11060664309660294F),
    glm::vec3(-0.016493938717834573F, -0.016493938717834257F, 1.2519364065950405F));
constexpr float k_agx_min_ev = -12.47393F; // log2(2^-10 * 0.18)
constexpr float k_agx_max_ev = 4.026069F;  // log2(2^6.5 * 0.18)

// sRGB (D65) <-> CIE XYZ and the Bradford cone response.
const glm::mat3 k_rgb_to_xyz = rows(0.4124564F, 0.3575761F, 0.1804375F, 0.2126729F, 0.7151522F,
                                    0.0721750F, 0.0193339F, 0.1191920F, 0.9503041F);
const glm::mat3 k_bradford = rows(0.8951F, 0.2664F, -0.1614F, -0.7502F, 1.7135F, 0.0367F, 0.0389F,
                                  -0.0685F, 1.0296F);

constexpr float k_reference_kelvin = 6500.0F;

[[nodiscard]] glm::vec3 xyz_of(glm::vec2 xy) {
    return {xy.x / xy.y, 1.0F, (1.0F - xy.x - xy.y) / xy.y};
}

// CIE 1960 uv of the Planckian locus (Krystek 1985) and its isotherm.
[[nodiscard]] glm::vec2 planckian_uv(float t) {
    const float u = (0.860117757F + 1.54118254e-4F * t + 1.28641212e-7F * t * t) /
                    (1.0F + 8.42420235e-4F * t + 7.08145163e-7F * t * t);
    const float v = (0.317398726F + 4.22806245e-5F * t + 4.20481691e-8F * t * t) /
                    (1.0F - 2.89741816e-5F * t + 1.61456053e-7F * t * t);
    return {u, v};
}

[[nodiscard]] glm::vec2 isotherm_direction(float t) {
    const float a = 1.41213984e6F + 1189.62F * t + t * t;
    const float b = 6.19363586e6F - 179.456F * t + t * t;
    const float ud = (-1.13758118e9F - 1.91615621e6F * t - 1.53177F * t * t) / (a * a);
    const float vd = (1.97471536e9F - 705674.0F * t - 308.607F * t * t) / (b * b);
    return glm::normalize(glm::vec2(ud, vd));
}

[[nodiscard]] glm::vec2 uv_to_xy(glm::vec2 uv) {
    const float d = 2.0F * uv.x - 8.0F * uv.y + 4.0F;
    return {3.0F * uv.x / d, 2.0F * uv.y / d};
}

// CIE daylight locus (valid 4000..25000 K).
[[nodiscard]] glm::vec2 daylight_xy(float t) {
    const float r = 1.0F / t;
    const float x = t <= 7000.0F
                        ? 0.244063F + (0.09911e3F + (2.9678e6F - 4.6070e9F * r) * r) * r
                        : 0.237040F + (0.24748e3F + (1.9018e6F - 2.0064e9F * r) * r) * r;
    return {x, -3.0F * x * x + 2.87F * x - 0.275F};
}

[[nodiscard]] float pow_signed(float x, float p) {
    return x < 0.0F ? -std::pow(-x, p) : std::pow(x, p);
}

} // namespace

glm::vec3 aces_narkowicz(glm::vec3 x) {
    constexpr float a = 2.51F, b = 0.03F, c = 2.43F, d = 0.59F, e = 0.14F;
    return glm::clamp((x * (a * x + b)) / (x * (c * x + d) + e), glm::vec3(0.0F), glm::vec3(1.0F));
}

float agx_contrast(float x) {
    const float x2 = x * x;
    const float x4 = x2 * x2;
    return 15.5F * x4 * x2 - 40.14F * x4 * x + 31.96F * x4 - 6.868F * x2 * x + 0.4298F * x2 +
           0.1191F * x - 0.00232F;
}

glm::vec3 agx(glm::vec3 x) {
    glm::vec3 c = k_agx_inset * (k_rec709_to_rec2020 * x);
    for (int i = 0; i < 3; ++i) {
        const float e = (std::log2(std::max(c[i], 1e-10F)) - k_agx_min_ev) /
                        (k_agx_max_ev - k_agx_min_ev);
        c[i] = agx_contrast(std::clamp(e, 0.0F, 1.0F));
    }
    c = k_agx_outset * c;
    for (int i = 0; i < 3; ++i) c[i] = std::pow(std::max(c[i], 0.0F), 2.2F);
    return glm::clamp(k_rec2020_to_rec709 * c, glm::vec3(0.0F), glm::vec3(1.0F));
}

glm::vec3 pbr_neutral(glm::vec3 color) {
    constexpr float start_compression = 0.8F - 0.04F;
    constexpr float desaturation = 0.15F;
    const float x = std::min(color.r, std::min(color.g, color.b));
    const float offset = x < 0.08F ? x - 6.25F * x * x : 0.04F;
    color -= offset;
    const float peak = std::max(color.r, std::max(color.g, color.b));
    if (peak < start_compression) return color;
    constexpr float d = 1.0F - start_compression;
    const float new_peak = 1.0F - d * d / (peak + d - start_compression);
    color *= new_peak / peak;
    const float g = 1.0F - 1.0F / (desaturation * (peak - new_peak) + 1.0F);
    return glm::mix(color, glm::vec3(new_peak), g);
}

glm::vec3 tonemap(TonemapOperator op, glm::vec3 x) {
    switch (op) {
    case TonemapOperator::agx: return agx(x);
    case TonemapOperator::pbr_neutral: return pbr_neutral(x);
    case TonemapOperator::aces: break;
    }
    return aces_narkowicz(x);
}

float linear_to_srgb(float c) {
    return c <= 0.0031308F ? c * 12.92F : 1.055F * std::pow(std::max(c, 0.0F), 1.0F / 2.4F) - 0.055F;
}

float srgb_to_linear(float c) {
    return c <= 0.04045F ? c / 12.92F : std::pow((c + 0.055F) / 1.055F, 2.4F);
}

float acescct_from_linear(float x) {
    if (x <= 0.0078125F) return 10.5402377416545F * x + 0.0729055341958355F;
    return (std::log2(x) + 9.72F) / 17.52F;
}

float linear_from_acescct(float x) {
    if (x <= 0.155251141552511F) return (x - 0.0729055341958355F) / 10.5402377416545F;
    return std::min(std::exp2(x * 17.52F - 9.72F), 65504.0F);
}

const glm::mat3& rec709_to_ap1() {
    static const glm::mat3 m = rows(0.6130974024F, 0.3395231462F, 0.0473794514F, 0.0701937225F,
                                    0.9163538791F, 0.0134523985F, 0.0206155929F, 0.1095697729F,
                                    0.8698146342F);
    return m;
}

const glm::mat3& ap1_to_rec709() {
    static const glm::mat3 m = rows(1.7048586763F, -0.6217160219F, -0.0832993717F, -0.1300768242F,
                                    1.1407357748F, -0.0105598017F, -0.0239640729F, -0.1289755083F,
                                    1.1530140189F);
    return m;
}

glm::vec2 white_chromaticity(float kelvin, float tint) {
    const float t = std::isfinite(kelvin) ? std::clamp(kelvin, 1000.0F, 25000.0F) : 6500.0F;
    const float tn = std::isfinite(tint) ? std::clamp(tint, -1.0F, 1.0F) : 0.0F;
    const glm::vec2 locus = planckian_uv(t);
    const glm::vec2 dir = isotherm_direction(t);
    // Tint moves perpendicular to the locus in uv (UE's convention: + = magenta).
    const glm::vec2 offset = uv_to_xy(locus + glm::vec2(-dir.y, dir.x) * (tn * 0.05F)) -
                             uv_to_xy(locus);
    const glm::vec2 base = t < 4000.0F ? uv_to_xy(locus) : daylight_xy(t);
    return base + offset;
}

glm::mat3 white_balance_matrix(float kelvin, float tint) {
    const glm::vec2 src = white_chromaticity(kelvin, tint);
    const glm::vec2 dst = white_chromaticity(k_reference_kelvin, 0.0F);
    if (src == dst) return glm::mat3(1.0F);
    const glm::vec3 lms_src = k_bradford * xyz_of(src);
    const glm::vec3 lms_dst = k_bradford * xyz_of(dst);
    glm::mat3 scale(1.0F);
    for (int i = 0; i < 3; ++i) scale[i][i] = lms_dst[i] / lms_src[i];
    const glm::mat3 adapt = glm::inverse(k_bradford) * scale * k_bradford;
    return glm::inverse(k_rgb_to_xyz) * adapt * k_rgb_to_xyz;
}

GradingGpu make_grading_gpu(const GradingSettings& s) {
    GradingGpu out{};
    const glm::mat3 wb = white_balance_matrix(s.temperature_k, s.tint);
    for (int r = 0; r < 3; ++r) out.white_balance[r] = glm::vec4(wb[0][r], wb[1][r], wb[2][r], 0.0F);
    const auto inv = [](float g) { return 1.0F / std::max(g, 0.01F); };
    out.lift = glm::vec4(s.lift_r, s.lift_g, s.lift_b, s.contrast);
    out.inv_gamma = glm::vec4(inv(s.gamma_r), inv(s.gamma_g), inv(s.gamma_b), s.saturation);
    out.gain = glm::vec4(s.gain_r, s.gain_g, s.gain_b, s.scene_neutral() ? 0.0F : 1.0F);
    return out;
}

glm::vec3 grade(const GradingGpu& p, glm::vec3 c) {
    if (p.gain.w == 0.0F) return c;
    const glm::mat3 wb = glm::transpose(glm::mat3(glm::vec3(p.white_balance[0]),
                                                  glm::vec3(p.white_balance[1]),
                                                  glm::vec3(p.white_balance[2])));
    glm::vec3 ap1 = rec709_to_ap1() * (wb * c);
    for (int i = 0; i < 3; ++i) {
        float v = acescct_from_linear(ap1[i]);
        v = (v - acescct_mid_grey) * p.lift.w + acescct_mid_grey;
        v = p.gain[i] * (v + p.lift[i] * (1.0F - v));
        v = pow_signed(v, p.inv_gamma[i]);
        ap1[i] = linear_from_acescct(v);
    }
    const float luma = glm::dot(ap1, ap1_luminance);
    ap1 = glm::vec3(luma) + (ap1 - glm::vec3(luma)) * p.inv_gamma.w;
    return glm::max(ap1_to_rec709() * ap1, glm::vec3(0.0F));
}

} // namespace engine::renderer::grading
