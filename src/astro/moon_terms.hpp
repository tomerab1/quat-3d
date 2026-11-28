#pragma once

// Periodic terms of the Moon's position (Meeus, "Astronomical Algorithms",
// 2nd ed., tables 47.A and 47.B). Private to src/astro.

#include <array>
#include <cstdint>

namespace engine::astro::detail {

// Multiples of D, M, M', F and the coefficients of sin (longitude, 1e-6°)
// and cos (distance, 1e-3 km).
struct MoonLrTerm {
    std::int8_t d, m, mp, f;
    std::int32_t l, r;
};
// Multiples of D, M, M', F and the coefficient of sin (latitude, 1e-6°).
struct MoonBTerm {
    std::int8_t d, m, mp, f;
    std::int32_t b;
};

extern const std::array<MoonLrTerm, 60> k_moon_lr_terms;
extern const std::array<MoonBTerm, 60>  k_moon_b_terms;

} // namespace engine::astro::detail
