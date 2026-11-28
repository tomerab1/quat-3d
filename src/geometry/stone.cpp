#include "engine/geometry/stone.hpp"

#include <algorithm>
#include <cmath>
#include <format>
#include <map>
#include <utility>

#include "engine/geometry/mesh_ops.hpp"

namespace engine::geometry {

namespace {

// SplitMix64 (Steele, Lea & Flood, "Fast splittable pseudorandom number
// generators", OOPSLA 2014): integer-only, so identical on every platform.
struct Rng {
    std::uint64_t state;
    std::uint64_t next() {
        std::uint64_t z = (state += 0x9E3779B97F4A7C15ULL);
        z = (z ^ (z >> 30)) * 0xBF58476D1CE4E5B9ULL;
        z = (z ^ (z >> 27)) * 0x94D049BB133111EBULL;
        return z ^ (z >> 31);
    }
    double uniform() { return static_cast<double>(next() >> 11) * 0x1.0p-53; } // [0, 1)
};

std::uint64_t mix(std::uint64_t h) {
    h ^= h >> 33;
    h *= 0xFF51AFD7ED558CCDULL;
    h ^= h >> 33;
    h *= 0xC4CEB9FE1A85EC53ULL;
    return h ^ (h >> 33);
}

// Lattice value in [-1, 1] at an integer point.
double lattice(std::int64_t x, std::int64_t y, std::int64_t z, std::uint64_t seed) {
    std::uint64_t h = mix(seed ^ 0x51ED27F1ULL);
    h = mix(h ^ static_cast<std::uint64_t>(x));
    h = mix(h ^ static_cast<std::uint64_t>(y));
    h = mix(h ^ static_cast<std::uint64_t>(z));
    return static_cast<double>(h >> 11) * 0x1.0p-52 - 1.0;
}

double fade(double t) { return t * t * t * (t * (t * 6.0 - 15.0) + 10.0); } // Perlin 2002 quintic

double value_noise(glm::dvec3 p, std::uint64_t seed) {
    const glm::dvec3 f = glm::floor(p);
    const auto x = static_cast<std::int64_t>(f.x);
    const auto y = static_cast<std::int64_t>(f.y);
    const auto z = static_cast<std::int64_t>(f.z);
    const glm::dvec3 t = p - f;
    const double u = fade(t.x);
    const double v = fade(t.y);
    const double w = fade(t.z);
    auto lerp = [](double a, double b, double s) { return a + (b - a) * s; };
    const double x00 = lerp(lattice(x, y, z, seed), lattice(x + 1, y, z, seed), u);
    const double x10 = lerp(lattice(x, y + 1, z, seed), lattice(x + 1, y + 1, z, seed), u);
    const double x01 = lerp(lattice(x, y, z + 1, seed), lattice(x + 1, y, z + 1, seed), u);
    const double x11 = lerp(lattice(x, y + 1, z + 1, seed), lattice(x + 1, y + 1, z + 1, seed), u);
    return lerp(lerp(x00, x10, v), lerp(x01, x11, v), w);
}

struct Icosphere {
    std::vector<glm::dvec3> points;
    std::vector<std::uint32_t> indices;
};

Icosphere icosphere(std::uint32_t level) {
    const double phi = (1.0 + std::sqrt(5.0)) * 0.5;
    Icosphere s;
    s.points = {{-1, phi, 0}, {1, phi, 0}, {-1, -phi, 0}, {1, -phi, 0}, {0, -1, phi}, {0, 1, phi},
                {0, -1, -phi}, {0, 1, -phi}, {phi, 0, -1}, {phi, 0, 1}, {-phi, 0, -1}, {-phi, 0, 1}};
    for (glm::dvec3& p : s.points) {
        p = glm::normalize(p);
    }
    s.indices = {0, 11, 5, 0, 5, 1, 0, 1, 7, 0, 7, 10, 0, 10, 11, 1, 5, 9, 5, 11, 4, 11, 10, 2, 10, 7, 6, 7, 1, 8,
                 3, 9, 4, 3, 4, 2, 3, 2, 6, 3, 6, 8, 3, 8, 9, 4, 9, 5, 2, 4, 11, 6, 2, 10, 8, 6, 7, 9, 8, 1};
    for (std::uint32_t l = 0; l < level; ++l) {
        std::map<std::pair<std::uint32_t, std::uint32_t>, std::uint32_t> mid;
        auto midpoint = [&](std::uint32_t a, std::uint32_t b) {
            const auto key = std::minmax(a, b);
            const auto [it, inserted] = mid.try_emplace({key.first, key.second}, 0U);
            if (inserted) {
                it->second = static_cast<std::uint32_t>(s.points.size());
                s.points.push_back(glm::normalize(s.points[a] + s.points[b]));
            }
            return it->second;
        };
        std::vector<std::uint32_t> next;
        next.reserve(s.indices.size() * 4);
        for (std::size_t t = 0; t < s.indices.size(); t += 3) {
            const std::uint32_t a = s.indices[t];
            const std::uint32_t b = s.indices[t + 1];
            const std::uint32_t c = s.indices[t + 2];
            const std::uint32_t ab = midpoint(a, b);
            const std::uint32_t bc = midpoint(b, c);
            const std::uint32_t ca = midpoint(c, a);
            next.insert(next.end(), {a, ab, ca, b, bc, ab, c, ca, bc, ab, bc, ca});
        }
        s.indices = std::move(next);
    }
    return s;
}

} // namespace

std::expected<MeshData, core::Error> generate_stone(const StoneOptions& o) {
    const bool sizes_ok = std::isfinite(o.size.x) && std::isfinite(o.size.y) && std::isfinite(o.size.z) &&
                          o.size.x > 0.0F && o.size.y > 0.0F && o.size.z > 0.0F && o.size.x <= 1000.0F &&
                          o.size.y <= 1000.0F && o.size.z <= 1000.0F;
    if (!sizes_ok) {
        return std::unexpected(core::make_error("stone: size must be finite, > 0 and <= 1000 m on every axis"));
    }
    if (o.subdivisions > 5 || o.noise_octaves > 8 || o.chips > 16) {
        return std::unexpected(core::make_error(std::format(
            "stone: subdivisions {} (max 5), noise_octaves {} (max 8) or chips {} (max 16) out of range",
            o.subdivisions, o.noise_octaves, o.chips)));
    }
    auto in01 = [](float v) { return std::isfinite(v) && v >= 0.0F && v <= 1.0F; };
    if (!in01(o.roundness) || !in01(o.noise_amplitude) || !in01(o.chip_depth) || !(o.uv_scale > 0.0F) ||
        !std::isfinite(o.uv_scale)) {
        return std::unexpected(core::make_error(
            "stone: roundness, noise_amplitude and chip_depth must be in [0, 1], uv_scale finite and > 0"));
    }

    Icosphere sphere = icosphere(o.subdivisions);
    const glm::dvec3 half = glm::dvec3(o.size) * 0.5;
    const double smallest = std::min({half.x, half.y, half.z});
    const double roundness = o.roundness;
    const std::uint64_t noise_seed = mix(o.seed);
    std::vector<glm::dvec3> pos(sphere.points.size());
    for (std::size_t i = 0; i < sphere.points.size(); ++i) {
        const glm::dvec3 d = sphere.points[i];
        // Blend the cube projection (box-like stone) with the sphere.
        const glm::dvec3 a = glm::abs(d);
        const glm::dvec3 cube = d / std::max({a.x, a.y, a.z});
        const glm::dvec3 shape = cube + (d - cube) * roundness;
        double n = 0.0;
        double amplitude = 1.0;
        double frequency = 2.0;
        double norm = 0.0;
        for (std::uint32_t k = 0; k < o.noise_octaves; ++k) {
            n += value_noise(d * frequency + glm::dvec3(17.0 * k), noise_seed + k) * amplitude;
            norm += amplitude;
            amplitude *= 0.5;
            frequency *= 2.0;
        }
        const double displacement = norm > 0.0 ? n / norm * o.noise_amplitude * smallest : 0.0;
        pos[i] = shape * half + d * displacement;
    }
    Rng rng{o.seed};
    for (std::uint32_t c = 0; c < o.chips; ++c) {
        glm::dvec3 n{0.0};
        for (int attempt = 0; attempt < 64; ++attempt) { // uniform direction by rejection
            n = {rng.uniform() * 2.0 - 1.0, rng.uniform() * 2.0 - 1.0, rng.uniform() * 2.0 - 1.0};
            const double l2 = glm::dot(n, n);
            if (l2 > 1e-6 && l2 <= 1.0) {
                n /= std::sqrt(l2);
                break;
            }
        }
        double support = 0.0;
        for (const glm::dvec3& p : pos) {
            support = std::max(support, glm::dot(p, n));
        }
        const double depth = o.chip_depth * (0.5 + 0.5 * rng.uniform());
        const double plane = support - depth * glm::length(half * glm::abs(n));
        for (glm::dvec3& p : pos) {
            const double over = glm::dot(p, n) - plane;
            if (over > 0.0) {
                p -= n * over;
            }
        }
    }

    MeshData mesh;
    mesh.vertices.resize(pos.size());
    for (std::size_t i = 0; i < pos.size(); ++i) {
        mesh.vertices[i].position = glm::vec3(pos[i]);
    }
    mesh.indices = std::move(sphere.indices);
    mesh.submeshes.push_back(asset::SubMesh{0, static_cast<std::uint32_t>(mesh.indices.size()), o.material});
    // Chips can collapse neighbouring vertices onto one point.
    WeldOptions weld_options;
    weld_options.position_only = true;
    weld(mesh, weld_options);
    UvOptions uv;
    uv.scale = o.uv_scale;
    generate_uvs(mesh, uv);
    NormalOptions normals;
    normals.crease_angle_degrees = o.crease_angle_degrees;
    compute_normals(mesh, normals);
    if (auto t = compute_tangents(mesh); !t) {
        return std::unexpected(t.error());
    }
    optimize(mesh);
    return mesh;
}

} // namespace engine::geometry
