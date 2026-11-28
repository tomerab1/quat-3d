#pragma once

// Bright star catalogue and star photometry (ROADMAP R22.2.1).
//
// The catalogue is the Yale Bright Star Catalogue (assets/sky/README.md),
// cooked offline by scripts/cook-star-catalogue.py into a small binary:
//   char[4] "QSC1", uint32 version 1, uint32 count, uint32 record size 20,
//   then per star float32 x, y, z (J2000 equatorial unit vector, x toward
//   the equinox, z toward the north celestial pole), V magnitude, B−V;
// little-endian. Files are untrusted input: parse_star_catalogue validates
// every field and the size before reading.
//
// Photometry (pure functions, no graphics):
//   - illuminance at normal incidence of a star of visual magnitude m,
//     E = 2.54·10⁻⁶ · 10^(−0.4 m) lux (m = 0 gives 2.54 µlux; Allen's
//     Astrophysical Quantities, the zero point Schaefer 1990 uses),
//   - B−V → effective temperature, Ballesteros, "New insights into
//     black bodies", EPL 97 (2012) 34008:
//     T = 4600 (1 / (0.92 (B−V) + 1.7) + 1 / (0.92 (B−V) + 0.62)) K,
//   - temperature → linear Rec.709 / sRGB chromaticity of the Planck
//     spectrum, integrated against the CIE 1931 2° observer through the
//     multi-lobe fit of Wyman, Sloan & Shirley, "Simple Analytic
//     Approximations to the CIE XYZ Color Matching Functions" (JCGT 2013),
//     normalised to luminance Y = 1 so colour times illuminance keeps the
//     photometric value.

#include <cstddef>
#include <cstdint>
#include <expected>
#include <span>
#include <string>
#include <vector>

#include <glm/glm.hpp>

#include "engine/core/error.hpp"

namespace engine::astro {

struct CatalogueStar {
    glm::vec3 direction{0.0F, 0.0F, 1.0F}; // J2000 equatorial unit vector
    float     v_mag = 0.0F;
    float     b_v = 0.0F;
};

inline constexpr std::uint32_t k_star_catalogue_version = 1;
inline constexpr std::size_t   k_star_catalogue_header_bytes = 16;
inline constexpr std::size_t   k_star_catalogue_record_bytes = 20;
// Generous cap (the Yale catalogue has 9110 entries; Hipparcos 118 218).
inline constexpr std::uint32_t k_star_catalogue_max_stars = 200000;

// Parses a cooked catalogue. Rejects a bad magic / version / record size, a
// size that does not match the count, more than k_star_catalogue_max_stars,
// non-finite fields, magnitudes outside [-30, 30], |B−V| > 5 and direction
// vectors that are not unit length (to 1e-3); the error names the star.
[[nodiscard]] std::expected<std::vector<CatalogueStar>, core::Error>
parse_star_catalogue(std::span<const std::byte> bytes);

// Reads and parses a catalogue file.
[[nodiscard]] std::expected<std::vector<CatalogueStar>, core::Error>
load_star_catalogue(const std::string& path);

// Serialises stars in the cooked format (tests and tools).
[[nodiscard]] std::vector<std::byte> encode_star_catalogue(std::span<const CatalogueStar> stars);

// Illuminance (lux) at normal incidence above the atmosphere.
[[nodiscard]] double star_illuminance_lux(double v_mag);
// Effective temperature (K) of a B−V colour index.
[[nodiscard]] double star_temperature_k(double b_v);
// Linear Rec.709 colour of a black body at `kelvin`, luminance Y = 1
// (negative components clamped, then renormalised).
[[nodiscard]] glm::vec3 blackbody_rgb(double kelvin);
// blackbody_rgb(star_temperature_k(b_v)).
[[nodiscard]] glm::vec3 star_rgb(double b_v);

} // namespace engine::astro
