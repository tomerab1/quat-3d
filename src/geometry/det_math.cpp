#include "geometry/det_math.hpp"

#include <cmath>

// Kernels from fdlibm 5.3 (k_sin.c, k_cos.c, s_atan.c):
//   Copyright (C) 1993 by Sun Microsystems, Inc. All rights reserved.
//   Developed at SunSoft, a Sun Microsystems, Inc. business. Permission to
//   use, copy, modify, and distribute this software is freely granted,
//   provided that this notice is preserved.
// Rewritten in C++ without the bit manipulation of the originals; the
// arithmetic is the same.

namespace engine::geometry::det {

namespace {

// pi/2 split for Cody-Waite reduction (fdlibm e_rem_pio2.c): pio2_1 has 33
// significant bits, so k * pio2_1 is exact for |k| < 2^20.
constexpr double pio2_1  = 1.57079632673412561417e+00;
constexpr double pio2_2  = 6.07710050630396597660e-11;
constexpr double pio2_3  = 2.02226624871116645580e-21;
constexpr double inv_pio2 = 6.36619772367581382433e-01;

// sin(x) on [-pi/4, pi/4].
double kernel_sin(double x) {
    constexpr double s1 = -1.66666666666666324348e-01;
    constexpr double s2 = 8.33333333332248946124e-03;
    constexpr double s3 = -1.98412698298579493134e-04;
    constexpr double s4 = 2.75573137070700676789e-06;
    constexpr double s5 = -2.50507602534068634195e-08;
    constexpr double s6 = 1.58969099521155010221e-10;
    const double z = x * x;
    const double v = z * x;
    const double r = s2 + z * (s3 + z * (s4 + z * (s5 + z * s6)));
    return x + v * (s1 + z * r);
}

// cos(x) on [-pi/4, pi/4].
double kernel_cos(double x) {
    constexpr double c1 = 4.16666666666666019037e-02;
    constexpr double c2 = -1.38888888888741095749e-03;
    constexpr double c3 = 2.48015872894767294178e-05;
    constexpr double c4 = -2.75573143513906633035e-07;
    constexpr double c5 = 2.08757232129817482790e-09;
    constexpr double c6 = -1.13596475577881948265e-11;
    const double z = x * x;
    const double r = z * (c1 + z * (c2 + z * (c3 + z * (c4 + z * (c5 + z * c6)))));
    const double hz = 0.5 * z;
    const double w = 1.0 - hz;
    return w + (((1.0 - w) - hz) + z * r);
}

// x = k * pi/2 + r with |r| <= ~pi/4; returns k mod 4.
int reduce(double x, double& r) {
    const double k = std::nearbyint(x * inv_pio2);
    r = ((x - k * pio2_1) - k * pio2_2) - k * pio2_3;
    const double q = k - 4.0 * std::floor(k * 0.25);
    return static_cast<int>(q);
}

} // namespace

void sincos(double x, double& s, double& c) {
    if (!std::isfinite(x)) {
        s = c = x - x; // NaN
        return;
    }
    double r = 0.0;
    const int q = reduce(x, r);
    const double ks = kernel_sin(r);
    const double kc = kernel_cos(r);
    switch (q) {
    case 0: s = ks; c = kc; break;
    case 1: s = kc; c = -ks; break;
    case 2: s = -ks; c = -kc; break;
    default: s = -kc; c = ks; break;
    }
}

double sin(double x) {
    double s = 0.0;
    double c = 0.0;
    sincos(x, s, c);
    return s;
}

double cos(double x) {
    double s = 0.0;
    double c = 0.0;
    sincos(x, s, c);
    return c;
}

double atan(double x) {
    constexpr double atanhi[] = {4.63647609000806093515e-01, 7.85398163397448278999e-01,
                                 9.82793723247329054082e-01, 1.57079632679489655800e+00};
    constexpr double atanlo[] = {2.26987774529616870924e-17, 3.06161699786838301793e-17,
                                 1.39033110312309984516e-17, 6.12323399573676603587e-17};
    constexpr double at[] = {3.33333333333329318027e-01, -1.99999999998764832476e-01,
                             1.42857142725034663711e-01, -1.11111104054623557880e-01,
                             9.09088713343650656196e-02, -7.69187620504482999495e-02,
                             6.66107313738753120669e-02, -5.83357013379057348645e-02,
                             4.97687799461593236017e-02, -3.65315727442169155270e-02,
                             1.62858201153657823623e-02};
    if (std::isnan(x)) {
        return x;
    }
    const bool negative = x < 0.0;
    double a = negative ? -x : x;
    if (a >= 3.6893488147419103e19) { // 2^65: atan is pi/2 to double precision
        const double v = atanhi[3] + atanlo[3];
        return negative ? -v : v;
    }
    int id = -1;
    if (a < 0.4375) {
        if (a < 3.7252902984e-09) { // 2^-28
            return x;
        }
    } else if (a < 1.1875) {
        if (a < 0.6875) {
            id = 0;
            a = (2.0 * a - 1.0) / (2.0 + a);
        } else {
            id = 1;
            a = (a - 1.0) / (a + 1.0);
        }
    } else if (a < 2.4375) {
        id = 2;
        a = (a - 1.5) / (1.0 + 1.5 * a);
    } else {
        id = 3;
        a = -1.0 / a;
    }
    const double z = a * a;
    const double w = z * z;
    const double s1 = z * (at[0] + w * (at[2] + w * (at[4] + w * (at[6] + w * (at[8] + w * at[10])))));
    const double s2 = w * (at[1] + w * (at[3] + w * (at[5] + w * (at[7] + w * at[9]))));
    double result = 0.0;
    if (id < 0) {
        result = a - a * (s1 + s2);
    } else {
        result = atanhi[id] - ((a * (s1 + s2) - atanlo[id]) - a);
    }
    return negative ? -result : result;
}

double atan2(double y, double x) {
    constexpr double pi_lo = 1.2246467991473531772e-16;
    if (std::isnan(x) || std::isnan(y)) {
        return x + y;
    }
    if (x == 0.0) {
        if (y == 0.0) {
            return 0.0;
        }
        return y > 0.0 ? half_pi : -half_pi;
    }
    if (y == 0.0) {
        return x > 0.0 ? 0.0 : pi;
    }
    if (x > 0.0) {
        return atan(y / x);
    }
    const double z = atan(std::fabs(y / x));
    // x < 0: shift by pi towards y's sign, carrying pi's low part (fdlibm
    // e_atan2.c).
    return y > 0.0 ? pi - (z - pi_lo) : (z - pi_lo) - pi;
}

double acos(double x) {
    if (std::isnan(x)) {
        return x;
    }
    if (x >= 1.0) {
        return 0.0;
    }
    if (x <= -1.0) {
        return pi;
    }
    return atan2(std::sqrt((1.0 - x) * (1.0 + x)), x);
}

} // namespace engine::geometry::det

// C-linkage entry points for the vendored MikkTSpace (vendor/mikktspace/mikktspace.c).
extern "C" double quat_geometry_det_cos(double x) { return engine::geometry::det::cos(x); }
extern "C" double quat_geometry_det_acos(double x) { return engine::geometry::det::acos(x); }
