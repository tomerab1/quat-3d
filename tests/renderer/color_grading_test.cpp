// R20 colour pipeline CPU references (engine/renderer/color_grading.hpp): the
// AgX curve and operator, Khronos PBR Neutral, ACEScct, the white-balance
// matrix and the scene-referred grade. tonemap_look_test.cpp checks the GPU
// against these.

#include <cmath>

#include <glm/glm.hpp>
#include <gtest/gtest.h>

#include "engine/renderer/color_grading.hpp"

namespace {

namespace r = engine::renderer;
namespace g = engine::renderer::grading;

float luminance(glm::vec3 c) { return glm::dot(c, glm::vec3(0.2126F, 0.7152F, 0.0722F)); }

TEST(ColorGrading, AgxContrastCurve) {
    // Wrensch's polynomial fit of Blender's AgX base contrast: ~0 at the
    // bottom of the log range, ~1 at the top, monotonic in between.
    EXPECT_NEAR(g::agx_contrast(0.0F), -0.00232F, 1e-6F);
    EXPECT_NEAR(g::agx_contrast(1.0F), 15.5F - 40.14F + 31.96F - 6.868F + 0.4298F + 0.1191F - 0.00232F,
                1e-4F);
    EXPECT_NEAR(g::agx_contrast(1.0F), 1.0F, 2e-3F);
    float previous = g::agx_contrast(0.0F);
    for (int i = 1; i <= 100; ++i) {
        const float v = g::agx_contrast(static_cast<float>(i) / 100.0F);
        EXPECT_GE(v, previous) << i;
        previous = v;
    }
    // Middle grey sits at (log2(0.18) + 12.47393) / 16.5 = 0.606 of the range
    // and lands in the display mid-tones.
    const float mid = (std::log2(0.18F) + 12.47393F) / (4.026069F + 12.47393F);
    EXPECT_NEAR(mid, 0.6060F, 1e-3F);
    EXPECT_GT(g::agx_contrast(mid), 0.4F);
    EXPECT_LT(g::agx_contrast(mid), 0.6F);
}

TEST(ColorGrading, AgxIsNeutralMonotonicAndDesaturatesHighlights) {
    // Greys stay grey (the inset / outset matrices preserve white).
    for (const float v : {0.01F, 0.18F, 1.0F, 10.0F}) {
        const glm::vec3 c = g::agx(glm::vec3(v));
        EXPECT_NEAR(c.r, c.g, 2e-3F) << v;
        EXPECT_NEAR(c.b, c.g, 2e-3F) << v;
    }
    // Monotonic in exposure, black to (almost) white: strictly inside the
    // log2 window (up to 6.5 stops over 0.18), flat at its top.
    float previous = -1.0F;
    for (float ev = -10.0F; ev <= 10.0F; ev += 0.5F) {
        const float y = luminance(g::agx(glm::vec3(0.18F * std::exp2(ev))));
        if (ev < 6.0F) {
            EXPECT_GT(y, previous) << ev;
        } else {
            EXPECT_GE(y, previous) << ev;
        }
        previous = y;
    }
    EXPECT_LT(luminance(g::agx(glm::vec3(0.0F))), 0.01F);
    EXPECT_GT(luminance(g::agx(glm::vec3(1000.0F))), 0.95F);
    // A very bright saturated red goes toward white (AgX's path to white), so
    // its green / blue rise well above zero, unlike a hue-preserving clip.
    const glm::vec3 hot = g::agx(glm::vec3(200.0F, 2.0F, 2.0F));
    EXPECT_GT(hot.g, 0.5F);
    EXPECT_GT(hot.r, hot.g);
    for (int i = 0; i < 3; ++i) EXPECT_LE(hot[i], 1.0F);
}

TEST(ColorGrading, PbrNeutralMatchesTheKhronosSpec) {
    // Below the compression start only the toe offset (0.04, or x - 6.25 x^2
    // for x < 0.08 in the darkest channel) is removed: base colours survive.
    const glm::vec3 c(0.5F, 0.3F, 0.2F);
    const glm::vec3 m = g::pbr_neutral(c);
    for (int i = 0; i < 3; ++i) EXPECT_NEAR(m[i], c[i] - 0.04F, 1e-6F);
    const glm::vec3 dark(0.06F, 0.1F, 0.2F);
    const float off = 0.06F - 6.25F * 0.06F * 0.06F;
    EXPECT_NEAR(g::pbr_neutral(dark).b, 0.2F - off, 1e-6F);
    // Above it: peak compressed below 1, hue kept, never above 1.
    const glm::vec3 bright = g::pbr_neutral(glm::vec3(4.0F, 2.0F, 1.0F));
    EXPECT_LT(bright.r, 1.0F);
    EXPECT_GT(bright.r, bright.g);
    EXPECT_GT(bright.g, bright.b);
    const glm::vec3 white = g::pbr_neutral(glm::vec3(1e5F));
    EXPECT_NEAR(white.r, 1.0F, 1e-3F);
}

TEST(ColorGrading, AcesCctRoundTripsAndPinsMidGrey) {
    EXPECT_NEAR(g::acescct_from_linear(0.18F), g::acescct_mid_grey, 1e-5F);
    for (const float x : {-0.01F, 0.0F, 0.005F, 0.0078125F, 0.18F, 1.0F, 50.0F, 200.0F}) {
        EXPECT_NEAR(g::linear_from_acescct(g::acescct_from_linear(x)), x,
                    1e-5F * std::max(1.0F, std::abs(x)))
            << x;
    }
    // The two segments meet at the break point (continuous).
    EXPECT_NEAR(10.5402377416545F * 0.0078125F + 0.0729055341958355F,
                (std::log2(0.0078125F) + 9.72F) / 17.52F, 1e-5F);
    // AP1 <-> Rec.709 are inverses and keep white.
    const glm::mat3 round = g::ap1_to_rec709() * g::rec709_to_ap1();
    for (int c = 0; c < 3; ++c) {
        for (int k = 0; k < 3; ++k) EXPECT_NEAR(round[c][k], c == k ? 1.0F : 0.0F, 1e-5F);
    }
    const glm::vec3 white = g::rec709_to_ap1() * glm::vec3(1.0F);
    EXPECT_NEAR(white.r, 1.0F, 1e-4F);
    EXPECT_NEAR(white.b, 1.0F, 1e-4F);
}

TEST(ColorGrading, WhiteBalanceNeutralisesTheChosenLight) {
    // 6500 K, no tint: exactly the identity.
    const glm::mat3 id = g::white_balance_matrix(6500.0F, 0.0F);
    for (int c = 0; c < 3; ++c) {
        for (int k = 0; k < 3; ++k) EXPECT_FLOAT_EQ(id[c][k], c == k ? 1.0F : 0.0F);
    }
    // Tungsten light (3200 K) rendered with a 3200 K white balance turns grey:
    // build the light's colour from its chromaticity relative to the 6500 K white.
    const auto rgb_of = [](glm::vec2 xy) {
        const glm::vec3 xyz(xy.x / xy.y, 1.0F, (1.0F - xy.x - xy.y) / xy.y);
        const glm::mat3 xyz_to_rgb = glm::inverse(glm::transpose(
            glm::mat3(0.4124564F, 0.3575761F, 0.1804375F, 0.2126729F, 0.7151522F, 0.0721750F,
                      0.0193339F, 0.1191920F, 0.9503041F)));
        return xyz_to_rgb * xyz;
    };
    const glm::vec3 reference = rgb_of(g::white_chromaticity(6500.0F, 0.0F));
    const glm::vec3 tungsten = rgb_of(g::white_chromaticity(3200.0F, 0.0F)) / reference;
    EXPECT_GT(tungsten.r, tungsten.b * 1.5F); // warm light
    const glm::vec3 balanced = g::white_balance_matrix(3200.0F, 0.0F) * tungsten;
    EXPECT_NEAR(balanced.r / balanced.g, 1.0F, 0.02F);
    EXPECT_NEAR(balanced.b / balanced.g, 1.0F, 0.02F);
    // So a 3200 K balance makes neutral (6500 K) light blue.
    const glm::vec3 cooled = g::white_balance_matrix(3200.0F, 0.0F) * glm::vec3(1.0F);
    EXPECT_GT(cooled.b, cooled.r);
    // Tint moves along the isotherm: +magenta lowers green relative to red and
    // blue, -green raises it, symmetrically.
    const glm::vec3 magenta = g::white_balance_matrix(6500.0F, 0.5F) * glm::vec3(1.0F);
    const glm::vec3 green = g::white_balance_matrix(6500.0F, -0.5F) * glm::vec3(1.0F);
    EXPECT_GT(magenta.g, 1.0F); // balancing out a magenta cast adds green ...
    EXPECT_LT(green.g, 1.0F);   // ... and a green cast removes it, by about as much
    EXPECT_NEAR((magenta.g - 1.0F) / (1.0F - green.g), 1.0F, 0.35F);
}

TEST(ColorGrading, GradeIsANoOpAtDefaultsAndBehavesPerControl) {
    const r::GradingSettings defaults;
    EXPECT_TRUE(defaults.scene_neutral());
    const g::GradingGpu neutral = g::make_grading_gpu(defaults);
    EXPECT_EQ(neutral.gain.w, 0.0F);
    const glm::vec3 c(0.3F, 0.18F, 0.05F);
    EXPECT_EQ(g::grade(neutral, c), c);

    // Contrast pivots on mid-grey: 0.18 stays, brighter gets brighter.
    r::GradingSettings s;
    s.contrast = 1.5F;
    const g::GradingGpu contrast = g::make_grading_gpu(s);
    EXPECT_NEAR(g::grade(contrast, glm::vec3(0.18F)).g, 0.18F, 1e-4F);
    EXPECT_GT(g::grade(contrast, glm::vec3(0.8F)).g, 0.8F);
    EXPECT_LT(g::grade(contrast, glm::vec3(0.05F)).g, 0.05F);

    // A neutral-but-flagged grade (gain 1 on purpose) round-trips exactly
    // enough through ACEScct to be invisible.
    s = {};
    s.saturation = 1.0F + 1e-7F;
    const g::GradingGpu identity_ish = g::make_grading_gpu(s);
    EXPECT_EQ(identity_ish.gain.w, 1.0F);
    const glm::vec3 round = g::grade(identity_ish, c);
    for (int i = 0; i < 3; ++i) EXPECT_NEAR(round[i], c[i], 1e-4F);

    // Saturation 0: grey with AP1 luminance; gain brightens; lift raises
    // blacks; per-channel gain tints.
    s = {};
    s.saturation = 0.0F;
    const glm::vec3 mono = g::grade(g::make_grading_gpu(s), c);
    EXPECT_NEAR(mono.r, mono.g, 1e-3F);
    EXPECT_NEAR(mono.b, mono.g, 1e-3F);
    s = {};
    s.gain_r = s.gain_g = s.gain_b = 1.1F;
    EXPECT_GT(g::grade(g::make_grading_gpu(s), c).g, c.g);
    s = {};
    s.lift_r = s.lift_g = s.lift_b = 0.05F;
    EXPECT_GT(g::grade(g::make_grading_gpu(s), glm::vec3(0.0F)).g, 0.001F);
    s = {};
    s.gain_b = 1.2F;
    const glm::vec3 bluer = g::grade(g::make_grading_gpu(s), glm::vec3(0.18F));
    EXPECT_GT(bluer.b, bluer.r);
    // Gamma > 1 lifts the mid-tones.
    s = {};
    s.gamma_r = s.gamma_g = s.gamma_b = 1.3F;
    EXPECT_GT(g::grade(g::make_grading_gpu(s), glm::vec3(0.18F)).g, 0.18F);
}

} // namespace
