// The cooked bright star catalogue and star photometry (R22.2.1): the parser
// rejects malformed (untrusted) files with a precise error, the shipped
// catalogue holds the Yale Bright Star Catalogue with Sirius where it
// belongs, magnitudes map to illuminance and B−V to plausible colours.

#include <cmath>
#include <cstddef>
#include <cstring>
#include <string>
#include <vector>

#include <glm/glm.hpp>
#include <gtest/gtest.h>

#include "engine/astro/coords.hpp"
#include "engine/astro/star_catalogue.hpp"
#include "support/expect_ok.hpp"

namespace {

namespace a = engine::astro;

[[nodiscard]] std::vector<a::CatalogueStar> two_stars() {
    return {{glm::vec3(1.0F, 0.0F, 0.0F), -1.46F, 0.0F},
            {glm::normalize(glm::vec3(0.0F, 0.6F, 0.8F)), 4.2F, 1.1F}};
}

// Overwrites the float at byte `at` of `bytes`.
void poke(std::vector<std::byte>& bytes, std::size_t at, float v) {
    std::memcpy(bytes.data() + at, &v, sizeof(v));
}

[[nodiscard]] float luminance(const glm::vec3& c) {
    return 0.2126F * c.r + 0.7152F * c.g + 0.0722F * c.b;
}

} // namespace

TEST(StarCatalogue, EncodeParseRoundTrips) {
    const auto stars = two_stars();
    auto parsed = a::parse_star_catalogue(a::encode_star_catalogue(stars));
    ASSERT_OK(parsed);
    ASSERT_EQ(parsed->size(), 2U);
    for (std::size_t i = 0; i < stars.size(); ++i) {
        EXPECT_FLOAT_EQ((*parsed)[i].v_mag, stars[i].v_mag);
        EXPECT_FLOAT_EQ((*parsed)[i].b_v, stars[i].b_v);
        EXPECT_NEAR(glm::length((*parsed)[i].direction - stars[i].direction), 0.0F, 1e-6F);
    }
}

TEST(StarCatalogue, RejectsMalformedFiles) {
    const std::vector<std::byte> good = a::encode_star_catalogue(two_stars());
    const auto expect_error = [](std::vector<std::byte> bytes, const char* what) {
        auto parsed = a::parse_star_catalogue(bytes);
        ASSERT_FALSE(parsed.has_value()) << what;
        EXPECT_NE(parsed.error().message.find(what), std::string::npos)
            << parsed.error().message;
    };
    expect_error({}, "truncated header");
    auto bad_magic = good;
    bad_magic[0] = std::byte{'X'};
    expect_error(bad_magic, "bad magic");
    auto truncated = good;
    truncated.pop_back();
    expect_error(truncated, "expected 56");
    auto huge = good;
    huge[8] = std::byte{0xFF};
    huge[9] = std::byte{0xFF};
    huge[10] = std::byte{0xFF};
    expect_error(huge, "more than the limit");
    auto nan = good;
    poke(nan, 16 + 20 + 12, NAN);
    expect_error(nan, "star 1: non-finite");
    auto not_unit = good;
    poke(not_unit, 16, 2.0F);
    expect_error(not_unit, "star 0: direction is not a unit vector");
    auto dim = good;
    poke(dim, 16 + 12, 99.0F);
    expect_error(dim, "star 0: magnitude out of range");
}

// The shipped catalogue: thousands of stars, brightest first, Sirius
// (α CMa, RA 6h45m09s, Dec −16°42′58″, V −1.46) at the top.
TEST(StarCatalogue, ShippedCatalogueStartsWithSirius) {
    auto stars = a::load_star_catalogue(std::string(QUAT_PROJECT_ROOT) +
                                        "/assets/sky/bright_stars.qsc");
    ASSERT_OK(stars);
    EXPECT_GT(stars->size(), 9000U);
    const a::CatalogueStar& sirius = stars->front();
    EXPECT_FLOAT_EQ(sirius.v_mag, -1.46F);
    const a::Equatorial eq = a::equatorial_from_vector(glm::dvec3(sirius.direction));
    EXPECT_NEAR(eq.right_ascension, (6.0 + 45.0 / 60.0 + 8.9 / 3600.0) * 15.0, 0.01);
    EXPECT_NEAR(eq.declination, -(16.0 + 42.0 / 60.0 + 58.0 / 3600.0), 0.01);
    for (std::size_t i = 1; i < stars->size(); ++i) {
        ASSERT_LE((*stars)[i - 1].v_mag, (*stars)[i].v_mag) << "not sorted at " << i;
    }
}

TEST(StarPhotometry, MagnitudesAreAFifthRootOfAHundredApart) {
    EXPECT_NEAR(a::star_illuminance_lux(0.0), 2.54e-6, 1e-9);
    // Five magnitudes are a factor of 100.
    EXPECT_NEAR(a::star_illuminance_lux(1.0) / a::star_illuminance_lux(6.0), 100.0, 1e-9);
    // Sirius (−1.46) against Vega (+0.03): 1.49 magnitudes, ×3.94.
    EXPECT_NEAR(a::star_illuminance_lux(-1.46) / a::star_illuminance_lux(0.03), 3.94, 0.01);
}

TEST(StarPhotometry, BvMapsToTemperatureAndColour) {
    // Ballesteros 2012: the Sun's B−V 0.65 is ~5800 K.
    EXPECT_NEAR(a::star_temperature_k(0.65), 5778.0, 60.0);
    EXPECT_GT(a::star_temperature_k(0.0), 9000.0);   // A0: Sirius, Vega
    EXPECT_LT(a::star_temperature_k(1.85), 3700.0);  // M2: Betelgeuse
    // Colours carry unit luminance (they multiply the photometric value).
    for (const double bv : {-0.3, 0.0, 0.65, 1.2, 1.85}) {
        EXPECT_NEAR(luminance(a::star_rgb(bv)), 1.0F, 1e-4F) << "B-V " << bv;
    }
    const glm::vec3 hot = a::star_rgb(-0.3);
    const glm::vec3 sun = a::star_rgb(0.65);
    const glm::vec3 cool = a::star_rgb(1.85);
    EXPECT_GT(hot.b, hot.r);   // blue-white
    EXPECT_GT(cool.r, cool.b); // orange-red
    // The Sun's black body is close to white in sRGB (D65 is 6504 K).
    EXPECT_NEAR(sun.r / sun.b, 1.2F, 0.25F);
}
