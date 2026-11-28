#include "engine/astro/star_catalogue.hpp"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <string>
#include <utility>

namespace engine::astro {

namespace {

[[nodiscard]] std::unexpected<core::Error> fail(std::string what) {
    return std::unexpected(core::Error{"star catalogue: " + std::move(what)});
}

constexpr std::array<char, 4> k_magic{'Q', 'S', 'C', '1'};

// Little-endian reads / writes (the format is little-endian on every host).
[[nodiscard]] std::uint32_t read_u32(std::span<const std::byte> b, std::size_t at) {
    std::uint32_t v = 0;
    for (std::size_t i = 0; i < 4; ++i) {
        v |= static_cast<std::uint32_t>(std::to_integer<std::uint8_t>(b[at + i])) << (8 * i);
    }
    return v;
}

[[nodiscard]] float read_f32(std::span<const std::byte> b, std::size_t at) {
    const std::uint32_t bits = read_u32(b, at);
    float v = 0.0F;
    std::memcpy(&v, &bits, sizeof(v));
    return v;
}

void write_u32(std::vector<std::byte>& out, std::uint32_t v) {
    for (std::size_t i = 0; i < 4; ++i) {
        out.push_back(static_cast<std::byte>((v >> (8 * i)) & 0xFFU));
    }
}

void write_f32(std::vector<std::byte>& out, float v) {
    std::uint32_t bits = 0;
    std::memcpy(&bits, &v, sizeof(bits));
    write_u32(out, bits);
}

// Wyman, Sloan & Shirley 2013, multi-lobe fit (eq. 2): a piecewise
// Gaussian with different widths either side of its peak.
[[nodiscard]] double lobe(double lambda, double mu, double sigma_lo, double sigma_hi) {
    const double s = lambda < mu ? sigma_lo : sigma_hi;
    const double t = (lambda - mu) / s;
    return std::exp(-0.5 * t * t);
}

[[nodiscard]] glm::dvec3 cie_xyz_bar(double lambda) {
    const double x = 1.056 * lobe(lambda, 599.8, 37.9, 31.0) +
                     0.362 * lobe(lambda, 442.0, 16.0, 26.7) -
                     0.065 * lobe(lambda, 501.1, 20.4, 26.2);
    const double y = 0.821 * lobe(lambda, 568.8, 46.9, 40.5) +
                     0.286 * lobe(lambda, 530.9, 16.3, 31.1);
    const double z = 1.217 * lobe(lambda, 437.0, 11.8, 36.0) +
                     0.681 * lobe(lambda, 459.0, 26.0, 13.8);
    return {x, y, z};
}

// Planck's law up to a constant factor (wavelength in nm).
[[nodiscard]] double planck(double lambda_nm, double kelvin) {
    constexpr double k_c2_nm_k = 1.4387769e7; // second radiation constant (nm K)
    const double l5 = std::pow(lambda_nm * 1e-3, 5.0); // in µm^5 to stay in range
    return 1.0 / (l5 * std::expm1(k_c2_nm_k / (lambda_nm * kelvin)));
}

} // namespace

std::expected<std::vector<CatalogueStar>, core::Error>
parse_star_catalogue(std::span<const std::byte> bytes) {
    if (bytes.size() < k_star_catalogue_header_bytes) return fail("truncated header");
    for (std::size_t i = 0; i < k_magic.size(); ++i) {
        if (std::to_integer<char>(bytes[i]) != k_magic[i]) return fail("bad magic (not QSC1)");
    }
    const std::uint32_t version = read_u32(bytes, 4);
    if (version != k_star_catalogue_version) {
        return fail("unsupported version " + std::to_string(version));
    }
    const std::uint32_t count = read_u32(bytes, 8);
    const std::uint32_t record = read_u32(bytes, 12);
    if (record != k_star_catalogue_record_bytes) {
        return fail("record size " + std::to_string(record) + ", expected 20");
    }
    if (count > k_star_catalogue_max_stars) {
        return fail(std::to_string(count) + " stars, more than the limit of " +
                    std::to_string(k_star_catalogue_max_stars));
    }
    const std::size_t expected =
        k_star_catalogue_header_bytes + static_cast<std::size_t>(count) * record;
    if (bytes.size() != expected) {
        return fail("size " + std::to_string(bytes.size()) + " bytes, expected " +
                    std::to_string(expected) + " for " + std::to_string(count) + " stars");
    }
    std::vector<CatalogueStar> stars;
    stars.reserve(count);
    for (std::uint32_t i = 0; i < count; ++i) {
        const std::size_t at = k_star_catalogue_header_bytes + static_cast<std::size_t>(i) * record;
        CatalogueStar s;
        s.direction = {read_f32(bytes, at), read_f32(bytes, at + 4), read_f32(bytes, at + 8)};
        s.v_mag = read_f32(bytes, at + 12);
        s.b_v = read_f32(bytes, at + 16);
        const std::string which = "star " + std::to_string(i);
        if (!std::isfinite(s.direction.x) || !std::isfinite(s.direction.y) ||
            !std::isfinite(s.direction.z) || !std::isfinite(s.v_mag) || !std::isfinite(s.b_v)) {
            return fail(which + ": non-finite field");
        }
        if (std::abs(glm::length(s.direction) - 1.0F) > 1e-3F) {
            return fail(which + ": direction is not a unit vector");
        }
        if (s.v_mag < -30.0F || s.v_mag > 30.0F) return fail(which + ": magnitude out of range");
        if (std::abs(s.b_v) > 5.0F) return fail(which + ": B-V out of range");
        s.direction = glm::normalize(s.direction);
        stars.push_back(s);
    }
    return stars;
}

std::expected<std::vector<CatalogueStar>, core::Error>
load_star_catalogue(const std::string& path) {
    std::ifstream in(path, std::ios::binary | std::ios::ate);
    if (!in) return fail("cannot open " + path);
    const std::streamoff size = in.tellg();
    constexpr std::streamoff k_max_bytes =
        static_cast<std::streamoff>(k_star_catalogue_header_bytes) +
        static_cast<std::streamoff>(k_star_catalogue_max_stars) *
            static_cast<std::streamoff>(k_star_catalogue_record_bytes);
    if (size < 0 || size > k_max_bytes) return fail(path + ": file size out of range");
    std::vector<std::byte> bytes(static_cast<std::size_t>(size));
    in.seekg(0);
    if (!bytes.empty() &&
        !in.read(reinterpret_cast<char*>(bytes.data()), static_cast<std::streamsize>(size))) {
        return fail("cannot read " + path);
    }
    auto stars = parse_star_catalogue(bytes);
    if (!stars) return std::unexpected(core::Error{path + ": " + stars.error().message});
    return stars;
}

std::vector<std::byte> encode_star_catalogue(std::span<const CatalogueStar> stars) {
    std::vector<std::byte> out;
    out.reserve(k_star_catalogue_header_bytes + stars.size() * k_star_catalogue_record_bytes);
    for (const char c : k_magic) out.push_back(static_cast<std::byte>(c));
    write_u32(out, k_star_catalogue_version);
    write_u32(out, static_cast<std::uint32_t>(stars.size()));
    write_u32(out, static_cast<std::uint32_t>(k_star_catalogue_record_bytes));
    for (const CatalogueStar& s : stars) {
        write_f32(out, s.direction.x);
        write_f32(out, s.direction.y);
        write_f32(out, s.direction.z);
        write_f32(out, s.v_mag);
        write_f32(out, s.b_v);
    }
    return out;
}

double star_illuminance_lux(double v_mag) {
    return 2.54e-6 * std::pow(10.0, -0.4 * v_mag);
}

double star_temperature_k(double b_v) {
    return 4600.0 * (1.0 / (0.92 * b_v + 1.7) + 1.0 / (0.92 * b_v + 0.62));
}

glm::vec3 blackbody_rgb(double kelvin) {
    const double t = std::clamp(std::isfinite(kelvin) ? kelvin : 5778.0, 1000.0, 50000.0);
    glm::dvec3 xyz{0.0};
    for (double lambda = 380.0; lambda <= 780.0; lambda += 2.5) {
        xyz += cie_xyz_bar(lambda) * planck(lambda, t);
    }
    // CIE XYZ -> linear Rec.709 (D65).
    const glm::dvec3 rgb{3.2404542 * xyz.x - 1.5371385 * xyz.y - 0.4985314 * xyz.z,
                         -0.9692660 * xyz.x + 1.8760108 * xyz.y + 0.0415560 * xyz.z,
                         0.0556434 * xyz.x - 0.2040259 * xyz.y + 1.0572252 * xyz.z};
    const glm::dvec3 clamped = glm::max(rgb, glm::dvec3(0.0));
    const double y = 0.2126 * clamped.r + 0.7152 * clamped.g + 0.0722 * clamped.b;
    return glm::vec3(clamped / std::max(y, 1e-300));
}

glm::vec3 star_rgb(double b_v) { return blackbody_rgb(star_temperature_k(b_v)); }

} // namespace engine::astro
