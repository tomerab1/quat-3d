#include "asset-gen/mesh_builder.hpp"

#include <algorithm>
#include <array>
#include <cmath>
#include <limits>
#include <map>
#include <utility>

namespace quat::asset_gen {

Primitive& Model::part(std::uint32_t material) {
    for (Primitive& p : primitives) {
        if (p.material == material) return p;
    }
    Primitive& p = primitives.emplace_back();
    p.material = material;
    return p;
}

std::size_t Model::triangle_count() const {
    std::size_t n = 0;
    for (const Primitive& p : primitives) n += p.triangle_count();
    return n;
}

void Model::bounds(glm::vec3& lo, glm::vec3& hi) const {
    lo = glm::vec3(std::numeric_limits<float>::max());
    hi = glm::vec3(std::numeric_limits<float>::lowest());
    for (const Primitive& p : primitives) {
        for (const glm::vec3& v : p.positions) {
            lo = glm::min(lo, v);
            hi = glm::max(hi, v);
        }
    }
}

Rng::Rng(std::uint64_t seed, std::uint64_t stream) : inc_((stream << 1U) | 1U) {
    next();
    state_ += seed;
    next();
}

std::uint32_t Rng::next() {
    const std::uint64_t old = state_;
    state_ = old * 6364136223846793005ULL + inc_;
    const auto xorshifted = static_cast<std::uint32_t>(((old >> 18U) ^ old) >> 27U);
    const auto rot = static_cast<std::uint32_t>(old >> 59U);
    return (xorshifted >> rot) | (xorshifted << ((32U - rot) & 31U));
}

float Rng::uniform(float lo, float hi) {
    // 24 random bits -> [0, 1) exactly representable.
    const float u = static_cast<float>(next() >> 8U) * (1.0F / 16777216.0F);
    return lo + (hi - lo) * u;
}

namespace {

std::uint32_t hash3(std::int32_t x, std::int32_t y, std::int32_t z, std::uint32_t seed) {
    std::uint32_t h = seed * 0x9E3779B9U;
    for (const std::int32_t v : {x, y, z}) {
        h ^= static_cast<std::uint32_t>(v) + 0x7F4A7C15U + (h << 6U) + (h >> 2U);
        h *= 0x85EBCA6BU;
        h ^= h >> 13U;
    }
    h *= 0xC2B2AE35U;
    h ^= h >> 16U;
    return h;
}

float lattice(std::int32_t x, std::int32_t y, std::int32_t z, std::uint32_t seed) {
    return static_cast<float>(hash3(x, y, z, seed) >> 8U) * (1.0F / 16777215.0F);
}

float smooth(float t) { return t * t * (3.0F - 2.0F * t); }

} // namespace

float value_noise(const glm::vec3& p, std::uint32_t seed) {
    const glm::vec3 f = glm::floor(p);
    const glm::ivec3 i(f);
    const glm::vec3 t(smooth(p.x - f.x), smooth(p.y - f.y), smooth(p.z - f.z));
    float c[2][2][2];
    for (int dz = 0; dz < 2; ++dz) {
        for (int dy = 0; dy < 2; ++dy) {
            for (int dx = 0; dx < 2; ++dx) {
                c[dz][dy][dx] = lattice(i.x + dx, i.y + dy, i.z + dz, seed);
            }
        }
    }
    const auto lerp = [](float a, float b, float s) { return a + (b - a) * s; };
    const float y0 = lerp(lerp(c[0][0][0], c[0][0][1], t.x), lerp(c[0][1][0], c[0][1][1], t.x), t.y);
    const float y1 = lerp(lerp(c[1][0][0], c[1][0][1], t.x), lerp(c[1][1][0], c[1][1][1], t.x), t.y);
    return lerp(y0, y1, t.z);
}

void add_triangle(Primitive& prim, const glm::vec3& a, const glm::vec3& b, const glm::vec3& c) {
    const glm::vec3 n = glm::cross(b - a, c - a);
    const float len = glm::length(n);
    if (!(len > 1.0e-10F)) return;
    const glm::vec3 unit = n / len;
    for (const glm::vec3& v : {a, b, c}) {
        prim.indices.push_back(static_cast<std::uint32_t>(prim.positions.size()));
        prim.positions.push_back(v);
        prim.normals.push_back(unit);
    }
}

Ring make_ring(const glm::vec3& centre, const glm::vec3& axis, float radius, int sides,
               float phase) {
    const glm::vec3 up = glm::normalize(axis);
    const glm::vec3 helper = std::abs(up.y) < 0.9F ? glm::vec3(0.0F, 1.0F, 0.0F)
                                                    : glm::vec3(1.0F, 0.0F, 0.0F);
    // Right-handed frame (u, up, w) so increasing angle runs counter-clockwise
    // seen from the tip of `axis`.
    const glm::vec3 w = glm::normalize(glm::cross(helper, up));
    const glm::vec3 u = glm::cross(up, w);
    Ring ring;
    ring.centre = centre;
    ring.points.reserve(static_cast<std::size_t>(sides));
    for (int i = 0; i < sides; ++i) {
        const float a = phase + 6.28318530718F * static_cast<float>(i) / static_cast<float>(sides);
        ring.points.push_back(centre + radius * (std::cos(a) * u - std::sin(a) * w));
    }
    return ring;
}

void add_band(Primitive& prim, const Ring& lower, const Ring& upper) {
    const std::size_t n = std::min(lower.points.size(), upper.points.size());
    for (std::size_t i = 0; i < n; ++i) {
        const std::size_t j = (i + 1) % n;
        add_triangle(prim, lower.points[i], lower.points[j], upper.points[j]);
        add_triangle(prim, lower.points[i], upper.points[j], upper.points[i]);
    }
}

void add_fan(Primitive& prim, const Ring& ring, const glm::vec3& apex, bool outward) {
    const std::size_t n = ring.points.size();
    for (std::size_t i = 0; i < n; ++i) {
        const std::size_t j = (i + 1) % n;
        if (outward) {
            add_triangle(prim, ring.points[i], ring.points[j], apex);
        } else {
            add_triangle(prim, ring.points[j], ring.points[i], apex);
        }
    }
}

void add_frustum(Primitive& prim, const glm::vec3& base, const glm::vec3& top, float base_radius,
                 float top_radius, int sides, float phase) {
    const glm::vec3 axis = top - base;
    const Ring lower = make_ring(base, axis, base_radius, sides, phase);
    const Ring upper = make_ring(top, axis, top_radius, sides, phase);
    add_band(prim, lower, upper);
    add_fan(prim, lower, base, false);
    if (top_radius > 0.0F) add_fan(prim, upper, top, true);
}

namespace {

struct Icosphere {
    std::vector<glm::vec3>                  dirs;
    std::vector<std::array<std::uint32_t, 3>> tris;
};

Icosphere make_icosphere(int subdivisions) {
    const float t = (1.0F + std::sqrt(5.0F)) * 0.5F;
    Icosphere s;
    for (const glm::vec3& v :
         {glm::vec3(-1, t, 0), glm::vec3(1, t, 0), glm::vec3(-1, -t, 0), glm::vec3(1, -t, 0),
          glm::vec3(0, -1, t), glm::vec3(0, 1, t), glm::vec3(0, -1, -t), glm::vec3(0, 1, -t),
          glm::vec3(t, 0, -1), glm::vec3(t, 0, 1), glm::vec3(-t, 0, -1), glm::vec3(-t, 0, 1)}) {
        s.dirs.push_back(glm::normalize(v));
    }
    s.tris = {{0, 11, 5}, {0, 5, 1},  {0, 1, 7},   {0, 7, 10}, {0, 10, 11},
              {1, 5, 9},  {5, 11, 4}, {11, 10, 2}, {10, 7, 6}, {7, 1, 8},
              {3, 9, 4},  {3, 4, 2},  {3, 2, 6},   {3, 6, 8},  {3, 8, 9},
              {4, 9, 5},  {2, 4, 11}, {6, 2, 10},  {8, 6, 7},  {9, 8, 1}};
    for (int level = 0; level < subdivisions; ++level) {
        std::map<std::pair<std::uint32_t, std::uint32_t>, std::uint32_t> mid;
        const auto midpoint = [&](std::uint32_t a, std::uint32_t b) {
            const auto key = std::minmax(a, b);
            if (const auto it = mid.find(key); it != mid.end()) return it->second;
            const auto idx = static_cast<std::uint32_t>(s.dirs.size());
            s.dirs.push_back(glm::normalize(s.dirs[a] + s.dirs[b]));
            mid.emplace(key, idx);
            return idx;
        };
        std::vector<std::array<std::uint32_t, 3>> next;
        next.reserve(s.tris.size() * 4);
        for (const auto& tri : s.tris) {
            const std::uint32_t ab = midpoint(tri[0], tri[1]);
            const std::uint32_t bc = midpoint(tri[1], tri[2]);
            const std::uint32_t ca = midpoint(tri[2], tri[0]);
            next.push_back({tri[0], ab, ca});
            next.push_back({tri[1], bc, ab});
            next.push_back({tri[2], ca, bc});
            next.push_back({ab, bc, ca});
        }
        s.tris = std::move(next);
    }
    return s;
}

} // namespace

void add_blob(Primitive& prim, const BlobParams& params) {
    const Icosphere sphere = make_icosphere(std::clamp(params.subdivisions, 0, 3));
    std::vector<glm::vec3> verts;
    verts.reserve(sphere.dirs.size());
    for (const glm::vec3& d : sphere.dirs) {
        const float n = value_noise(d * params.frequency + glm::vec3(17.0F), params.seed);
        const float push = 1.0F + params.roughness * (2.0F * n - 1.0F);
        glm::vec3 v = params.centre + d * params.radii * push;
        v.y = std::max(v.y, params.floor_y);
        verts.push_back(v);
    }
    for (const auto& tri : sphere.tris) {
        add_triangle(prim, verts[tri[0]], verts[tri[1]], verts[tri[2]]);
    }
}

} // namespace quat::asset_gen
