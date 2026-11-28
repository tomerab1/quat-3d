// Photometric calibration and the auto-exposure meter's CPU reference
// (R22.2.1): EV100 relations, histogram binning over the 40 EV range, the
// percentile metering, log-domain adaptation and the adaptive key, and that
// the meter converges to the right exposure from a 10⁻³ lux night to a
// 10⁵ lux noon.

#include <array>
#include <cmath>
#include <cstdint>

#include <gtest/gtest.h>

#include "engine/renderer/photometry.hpp"
#include "engine/renderer/physical_camera.hpp"
#include "engine/renderer/render_settings.hpp"

namespace {

namespace r = engine::renderer;
namespace ph = engine::renderer::photometry;

using Bins = std::array<std::uint32_t, ph::k_histogram_bins>;

// Luminance (HDR units) of a grey card of albedo 0.18 under `lux`.
[[nodiscard]] float grey_card_units(float lux) {
    return 0.18F * lux / 3.14159265F / ph::k_nits_per_unit;
}

// Histogram of `pixels` copies of one luminance.
[[nodiscard]] Bins uniform(float units, std::uint32_t pixels = 1000) {
    Bins b{};
    b[ph::histogram_bin(units)] = pixels;
    return b;
}

// The fixed point of the meter for a uniform scene: adapted luminance and the
// exposure it maps to (default settings).
[[nodiscard]] float converged_exposure(float units) {
    const Bins b = uniform(units);
    float state = 0.0F;
    for (int i = 0; i < 200; ++i) {
        state = ph::adapt_log2(i == 0 ? NAN : state, ph::metered_log2(b), 1.0F / 60.0F, 1.1F);
    }
    const r::ExposureSettings s;
    return ph::auto_exposure(std::exp2(state), s.key, s.adaptive_key, s.min_ev100, s.max_ev100);
}

} // namespace

TEST(Photometry, CalibrationPutsTheSkyLutSunAtItsIlluminance) {
    // 8 units (the sky-view LUT's sun) are the extra-atmospheric 128 klux.
    EXPECT_FLOAT_EQ(ph::k_nits_per_unit * ph::k_sky_lut_scale, ph::k_sun_reference_lux);
}

TEST(Photometry, Ev100MatchesTheMeterEquation) {
    // L = 12.5 cd/m² meters at EV100 = log2(12.5 * 100 / 12.5) = log2(100).
    EXPECT_NEAR(ph::ev100_from_luminance(12.5F), std::log2(100.0F), 1e-5F);
    // Sunny 16: a sunlit scene (~4000 cd/m²) is about EV100 15.
    EXPECT_NEAR(ph::ev100_from_luminance(4096.0F), 15.0F, 0.1F);
    // Lagarde & de Rousiers 2014: exposure 1 / (1.2 * 2^EV100) per cd/m².
    EXPECT_NEAR(ph::exposure_from_ev100_units(0.0F), ph::k_nits_per_unit / 1.2F, 1e-2F);
    EXPECT_NEAR(ph::exposure_from_ev100_units(10.0F) / ph::exposure_from_ev100_units(11.0F),
                2.0F, 1e-5F);
}

TEST(Photometry, HistogramBinsCoverFortyStops) {
    EXPECT_EQ(ph::histogram_bin(0.0F), 0U);
    EXPECT_EQ(ph::histogram_bin(NAN), 0U);
    EXPECT_EQ(ph::histogram_bin(-1.0F), 0U);
    EXPECT_EQ(ph::histogram_bin(std::exp2(ph::k_histogram_min_log2 - 1.0F)), 0U);
    EXPECT_EQ(ph::histogram_bin(std::exp2(ph::k_histogram_max_log2 + 5.0F)), 255U);
    // Every bin's centre maps back to that bin.
    for (std::uint32_t b = 1; b < ph::k_histogram_bins; ++b) {
        EXPECT_EQ(ph::histogram_bin(std::exp2(ph::histogram_bin_log2(b))), b) << "bin " << b;
    }
    // A 10⁻⁴ lux night and a 10⁵ lux noon both fall inside the range.
    EXPECT_GT(ph::histogram_bin(grey_card_units(1e-4F)), 1U);
    EXPECT_LT(ph::histogram_bin(grey_card_units(1e5F)), 255U);
}

TEST(Photometry, MeteringIgnoresTheTailsOutsideThePercentiles) {
    Bins b{};
    const std::uint32_t mid = ph::histogram_bin(0.2F);
    b[mid] = 900;
    b[ph::histogram_bin(4000.0F)] = 50; // specular glints: top 5 %
    b[ph::histogram_bin(1e-6F)] = 50;   // black corners: bottom 5 %
    EXPECT_NEAR(ph::metered_log2(b), ph::histogram_bin_log2(mid), 1e-4F);
    // All black: nothing to meter.
    Bins black{};
    black[0] = 100;
    EXPECT_TRUE(std::isnan(ph::metered_log2(black)));
}

TEST(Photometry, AdaptationIsExponentialInTheLogDomain) {
    const float tau = 1.1F;
    const float a = ph::adapt_log2(-10.0F, 0.0F, tau, tau);
    EXPECT_NEAR(a, -10.0F * std::exp(-1.0F), 1e-4F);
    EXPECT_FLOAT_EQ(ph::adapt_log2(NAN, 3.0F, 0.1F, tau), 3.0F);  // seeds
    EXPECT_FLOAT_EQ(ph::adapt_log2(2.0F, NAN, 0.1F, tau), 2.0F);  // keeps a sane value
}

TEST(Photometry, AdaptiveKeyDimsNightsAndKeepsDaylight) {
    const float key = 0.18F;
    // Daylight (4000 cd/m²) keeps the user key; a night is shown dimmer.
    EXPECT_FLOAT_EQ(ph::adaptive_key(4000.0F / ph::k_nits_per_unit, key, true), key);
    const float night = ph::adaptive_key(1e-3F / ph::k_nits_per_unit, key, true);
    EXPECT_LT(night, key * 0.1F);
    EXPECT_GT(night, 0.0F);
    // Monotone between.
    float prev = 0.0F;
    for (float nits = 1e-4F; nits < 1e4F; nits *= 3.0F) {
        const float k = ph::adaptive_key(nits / ph::k_nits_per_unit, key, true);
        EXPECT_GE(k, prev) << nits << " cd/m²";
        prev = k;
    }
    EXPECT_FLOAT_EQ(ph::adaptive_key(1e-9F, key, false), key);
}

// The meter converges on a 10⁻³ lux night and a 10⁵ lux noon to the
// exposure that maps the scene to its (adaptive) key, inside the default
// EV100 range (neither clamps).
TEST(Photometry, ConvergesFromMillilxNightsToHundredKiloluxNoons) {
    const r::ExposureSettings s;
    for (const float lux : {1e-3F, 1e5F}) {
        const float scene = grey_card_units(lux);
        const float e = converged_exposure(scene);
        const float expected = ph::adaptive_key(scene, s.key, true) / scene;
        // One histogram bin (0.157 EV) of quantisation at most.
        EXPECT_NEAR(std::log2(e), std::log2(expected), 0.16F) << lux << " lux";
        const float ev = ph::ev100_from_luminance(scene * ph::k_nits_per_unit);
        EXPECT_GT(ev, s.min_ev100) << lux << " lux";
        EXPECT_LT(ev, s.max_ev100) << lux << " lux";
    }
    // The noon scene displays at the key; the night one darker but not black.
    const float noon = grey_card_units(1e5F);
    EXPECT_NEAR(noon * converged_exposure(noon), s.key, s.key * 0.12F);
    const float night = grey_card_units(1e-3F);
    const float shown = night * converged_exposure(night);
    EXPECT_GT(shown, 0.002F);
    EXPECT_LT(shown, 0.02F);
}

TEST(Photometry, AutoExposureIsClampedToTheEv100Range) {
    // A scene far darker than min EV100 is exposed as min EV100.
    const float e = ph::auto_exposure(1e-12F, 0.18F, false, -6.0F, 20.0F);
    EXPECT_NEAR(e, ph::exposure_from_ev100_units(-6.0F), e * 1e-5F);
    const float b = ph::auto_exposure(1e6F, 0.18F, false, -6.0F, 20.0F);
    EXPECT_NEAR(b, ph::exposure_from_ev100_units(20.0F), b * 1e-5F);
}
