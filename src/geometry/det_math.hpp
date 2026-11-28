#pragma once

// Deterministic elementary functions for the geometry kernel (ROADMAP R23.1).
//
// libm's sin / cos / atan2 / acos are not specified to the last bit and do
// differ between glibc, Apple's libm and the MSVC runtime, so generated
// meshes would hash differently per platform. These use only + - * / and
// IEEE-754 sqrt (correctly rounded everywhere) and are compiled with
// floating-point contraction off (CMakeLists.txt), so they return identical
// bits on every platform. Accuracy is within a few ulp of the true value.
//
// Kernels and coefficients are fdlibm's (Sun Microsystems, 1993: k_sin.c,
// k_cos.c, s_atan.c), with Cody & Waite (1980) range reduction by pi/2 split
// into three parts. Arguments are meant for geometry (|x| < ~1e5 radians);
// larger ones stay deterministic but lose accuracy.

namespace engine::geometry::det {

inline constexpr double pi      = 3.14159265358979311600e+00;
inline constexpr double half_pi = 1.57079632679489655800e+00;
inline constexpr double two_pi  = 6.28318530717958623200e+00;

[[nodiscard]] double sin(double x);
[[nodiscard]] double cos(double x);
// Both at once (one range reduction).
void sincos(double x, double& s, double& c);
[[nodiscard]] double atan(double x);
// atan2 with the usual quadrant conventions; atan2(0, 0) = 0.
[[nodiscard]] double atan2(double y, double x);
// acos for x in [-1, 1] (clamped).
[[nodiscard]] double acos(double x);

// Degrees -> radians with a fixed constant (no library call).
[[nodiscard]] constexpr double radians(double degrees) { return degrees * (pi / 180.0); }

} // namespace engine::geometry::det
