#pragma once

// Colour pipeline of the final image (ROADMAP R20): CPU references of the
// tonemap operators and the grading math that tonemap.slang implements, the
// white-balance matrix the tonemap pass computes per frame, and the packed
// parameters it uploads. The tests compare the GPU output with these.
//
// Pipeline (linear Rec.709 / sRGB primaries in, display-encoded sRGB out):
//   exposure -> white balance (von Kries, Bradford LMS) -> ACEScct:
//   contrast around mid-grey, lift / gamma / gain per channel -> linear AP1:
//   saturation -> Rec.709 -> tonemap operator -> sRGB encode -> look LUT.

#include <glm/glm.hpp>

#include "engine/renderer/post_settings.hpp"

namespace engine::renderer::grading {

// ---- Tonemap operators (linear Rec.709 in, display-linear [0, 1] out) ------

// Narkowicz's fit of the ACES RRT + ODT (the engine's original operator).
[[nodiscard]] glm::vec3 aces_narkowicz(glm::vec3 x);

// AgX's sigmoid in its log2 encoding (x in [0, 1]): the 6th-order polynomial
// fit of Blender's default "base contrast" curve (Sobotka's AgX as Blender 4
// ships it; the fit is Benjamin Wrensch's, used by Filament and three.js).
[[nodiscard]] float agx_contrast(float x);
// AgX: Rec.709 -> Rec.2020, inset matrix, log2 encoding over [-12.47, 4.03]
// stops around 0.18, the sigmoid, outset matrix (Blender 4 restores purity),
// 2.2 display decode, back to Rec.709, clamped to [0, 1].
[[nodiscard]] glm::vec3 agx(glm::vec3 x);

// Khronos PBR Neutral (2024): exact below 0.76, a soft shoulder above that
// preserves hue and desaturates only toward white.
[[nodiscard]] glm::vec3 pbr_neutral(glm::vec3 x);

[[nodiscard]] glm::vec3 tonemap(TonemapOperator op, glm::vec3 x);

// sRGB transfer function (display encoding).
[[nodiscard]] float linear_to_srgb(float c);
[[nodiscard]] float srgb_to_linear(float c);

// ---- Scene-referred grading -------------------------------------------------

// ACEScct (Academy S-2016-001) encoding of a linear AP1 value and back.
[[nodiscard]] float acescct_from_linear(float x);
[[nodiscard]] float linear_from_acescct(float x);
inline constexpr float acescct_mid_grey = 0.4135884F; // ACEScct(0.18)

// Primaries conversions (Bradford-adapted D65 <-> ACES white).
[[nodiscard]] const glm::mat3& rec709_to_ap1();
[[nodiscard]] const glm::mat3& ap1_to_rec709();
inline constexpr glm::vec3 ap1_luminance{0.2722287F, 0.6740818F, 0.0536895F};

// CIE 1931 xy chromaticity of a light of colour temperature `kelvin` (the
// Planckian locus below 4000 K, the CIE daylight locus above), offset along
// the isotherm by `tint` (+1 magenta .. -1 green, 0.05 duv per unit).
[[nodiscard]] glm::vec2 white_chromaticity(float kelvin, float tint);

// Linear Rec.709 white-balance matrix: von Kries adaptation in Bradford LMS
// from the white of (kelvin, tint) to the white of (6500 K, 0), so a light of
// that colour renders neutral; the identity at the defaults.
[[nodiscard]] glm::mat3 white_balance_matrix(float kelvin, float tint);

// The grading parameters as tonemap.slang's PostParams stores them
// (std140-compatible: every member a float4).
struct GradingGpu {
    glm::vec4 white_balance[3]; // rows of the 3x3 matrix (w unused)
    glm::vec4 lift;             // rgb lift, w contrast
    glm::vec4 inv_gamma;        // rgb 1 / gamma, w saturation
    glm::vec4 gain;             // rgb gain, w 1 = grade (0: scene-neutral, skipped)
};
static_assert(sizeof(GradingGpu) == 96);

[[nodiscard]] GradingGpu make_grading_gpu(const GradingSettings& settings);

// CPU reference of tonemap.slang's grade() on a linear Rec.709 colour.
[[nodiscard]] glm::vec3 grade(const GradingGpu& params, glm::vec3 linear);

} // namespace engine::renderer::grading
