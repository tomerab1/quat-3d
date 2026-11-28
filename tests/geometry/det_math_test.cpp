#include <bit>
#include <cmath>
#include <cstdint>

#include <gtest/gtest.h>

#include "geometry/det_math.hpp"

namespace det = engine::geometry::det;

namespace {

double ulp_distance(double a, double b) {
    const double scale = std::max(std::fabs(b), 1.0);
    return std::fabs(a - b) / (scale * 2.220446049250313e-16);
}

} // namespace

TEST(GeometryDetMath, MatchesLibmWithinAFewUlp) {
    double worst_sin = 0.0;
    double worst_cos = 0.0;
    double worst_atan2 = 0.0;
    double worst_acos = 0.0;
    for (int i = -20000; i <= 20000; ++i) {
        const double x = i * 0.0031415;
        worst_sin = std::max(worst_sin, ulp_distance(det::sin(x), std::sin(x)));
        worst_cos = std::max(worst_cos, ulp_distance(det::cos(x), std::cos(x)));
        const double y = (i % 97) * 0.37 - 17.0;
        worst_atan2 = std::max(worst_atan2, ulp_distance(det::atan2(y, x), std::atan2(y, x)));
        const double c = i / 20000.0;
        worst_acos = std::max(worst_acos, ulp_distance(det::acos(c), std::acos(c)));
    }
    EXPECT_LT(worst_sin, 2.0);
    EXPECT_LT(worst_cos, 2.0);
    EXPECT_LT(worst_atan2, 4.0);
    EXPECT_LT(worst_acos, 4.0);
}

TEST(GeometryDetMath, SpecialValues) {
    EXPECT_EQ(det::sin(0.0), 0.0);
    EXPECT_EQ(det::cos(0.0), 1.0);
    EXPECT_EQ(det::atan2(0.0, 0.0), 0.0);
    EXPECT_EQ(det::atan2(0.0, -1.0), det::pi);
    EXPECT_EQ(det::atan2(1.0, 0.0), det::half_pi);
    EXPECT_EQ(det::acos(1.0), 0.0);
    EXPECT_EQ(det::acos(-1.0), det::pi);
    EXPECT_EQ(det::acos(2.0), 0.0); // clamped
    EXPECT_TRUE(std::isnan(det::sin(std::nan(""))));
    EXPECT_TRUE(std::isnan(det::cos(INFINITY)));
}

// The same bits on every compiler and OS: a hash of many results is a
// golden value (computed on Linux / clang; CI checks Windows and macOS).
TEST(GeometryDetMath, BitIdenticalGolden) {
    std::uint64_t h = 14695981039346656037ULL;
    auto feed = [&](double v) {
        const auto bits = std::bit_cast<std::uint64_t>(v);
        for (int k = 0; k < 8; ++k) {
            h = (h ^ ((bits >> (8 * k)) & 0xFF)) * 1099511628211ULL;
        }
    };
    for (int i = -500; i <= 500; ++i) {
        const double x = i * 0.0137;
        feed(det::sin(x));
        feed(det::cos(x));
        feed(det::atan(x * 7.0));
        feed(det::atan2(x, 0.5 - x));
        feed(det::acos(i / 500.0));
    }
    EXPECT_EQ(h, 0x7fb026a3b69bb87bULL) << std::hex << "actual 0x" << h;
}
