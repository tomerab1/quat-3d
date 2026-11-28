// Spline validation and compilation into cubic Bézier segments (ROADMAP G18.1).
//
// Centripetal Catmull-Rom: C. Yuksel, S. Schaefer, J. Keyser, "Parameterization
// and applications of Catmull-Rom curves", Computer-Aided Design 43(7), 2011.
// Knots are spaced by |P[i+1] - P[i]|^alpha with alpha = 1/2; each segment
// is the Barry–Goldman pyramid's cubic, written here in its equivalent Hermite
// form (tangents from the non-uniform finite-difference formula, scaled to the
// segment's knot interval) and converted to Bézier control points.

#include <cmath>
#include <cstring>
#include <string>

#include "engine/spline/spline.hpp"

namespace engine::spline {

namespace {

constexpr float k_max_height = k_max_width;
constexpr float k_max_ground_offset = 1.0e4F;

struct KindName {
    std::string_view name;
    SplineKind       kind;
};
constexpr KindName k_kinds[] = {{"centripetal_catmull_rom", SplineKind::centripetal_catmull_rom},
                                {"cubic_bezier", SplineKind::cubic_bezier},
                                {"polyline", SplineKind::polyline}};

[[nodiscard]] bool finite_in(const glm::vec3& v, float limit) {
    for (int i = 0; i < 3; ++i) {
        if (!std::isfinite(v[i]) || std::abs(v[i]) > limit) return false;
    }
    return true;
}

[[nodiscard]] std::string point_field(std::size_t i, std::string_view field) {
    return "points[" + std::to_string(i) + "]." + std::string(field);
}

// sqrt(|d|): the centripetal knot gap (alpha = 1/2), as two correctly
// rounded square roots so it is bit-identical everywhere (no pow()).
[[nodiscard]] float knot_gap(const glm::vec3& a, const glm::vec3& b) {
    const glm::vec3 d = b - a;
    return std::sqrt(std::sqrt(d.x * d.x + d.y * d.y + d.z * d.z));
}

// Tangent (dP/dt in knot units) at P1 of the knot sequence P0, P1, P2 with
// gaps g01, g12; a zero gap falls back to the chord P2 - P1 per unit gap.
[[nodiscard]] glm::vec3 centripetal_tangent(const glm::vec3& p0, const glm::vec3& p1, const glm::vec3& p2,
                                            float g01, float g12) {
    if (g12 <= 0.0F) return glm::vec3(0.0F);
    if (g01 <= 0.0F) return (p2 - p1) / g12;
    return (p1 - p0) / g01 - (p2 - p0) / (g01 + g12) + (p2 - p1) / g12;
}

// Same at P2 of P1, P2, P3 (the end tangent of segment P1 -> P2).
[[nodiscard]] glm::vec3 centripetal_end_tangent(const glm::vec3& p1, const glm::vec3& p2, const glm::vec3& p3,
                                                float g12, float g23) {
    if (g12 <= 0.0F) return glm::vec3(0.0F);
    if (g23 <= 0.0F) return (p2 - p1) / g12;
    return (p2 - p1) / g12 - (p3 - p1) / (g12 + g23) + (p3 - p2) / g23;
}

void set_attributes(Segment& s, const SplinePoint& a, const SplinePoint& b, std::uint32_t first) {
    s.width[0] = a.width;
    s.width[1] = b.width;
    s.height[0] = a.height;
    s.height[1] = b.height;
    s.tags = a.tags;
    s.first_point = first;
}

[[nodiscard]] std::size_t min_points(const Spline& s) {
    switch (s.kind) {
    case SplineKind::cubic_bezier: return s.closed ? 3U : 4U;
    case SplineKind::centripetal_catmull_rom:
    case SplineKind::polyline: break;
    }
    return s.closed ? 3U : 2U;
}

void build_catmull_rom(const Spline& s, std::vector<Segment>& out) {
    const auto& pts = s.points;
    const std::size_t n = pts.size();
    const std::size_t count = s.closed ? n : n - 1;
    // The point at index i, with open ends extended by reflection.
    auto at = [&](std::ptrdiff_t i) -> glm::vec3 {
        const auto sn = static_cast<std::ptrdiff_t>(n);
        if (s.closed) return pts[static_cast<std::size_t>(((i % sn) + sn) % sn)].position;
        if (i < 0) return pts[0].position * 2.0F - pts[1].position;
        if (i >= sn) return pts[n - 1].position * 2.0F - pts[n - 2].position;
        return pts[static_cast<std::size_t>(i)].position;
    };
    out.reserve(count);
    for (std::size_t i = 0; i < count; ++i) {
        const auto si = static_cast<std::ptrdiff_t>(i);
        const glm::vec3 p0 = at(si - 1);
        const glm::vec3 p1 = at(si);
        const glm::vec3 p2 = at(si + 1);
        const glm::vec3 p3 = at(si + 2);
        const float g01 = knot_gap(p0, p1);
        const float g12 = knot_gap(p1, p2);
        const float g23 = knot_gap(p2, p3);
        Segment seg;
        seg.p[0] = p1;
        seg.p[3] = p2;
        // Hermite -> Bézier: tangents in knot units times the segment's knot
        // interval give d/du; the inner control points sit a third along.
        const glm::vec3 m1 = centripetal_tangent(p0, p1, p2, g01, g12) * g12;
        const glm::vec3 m2 = centripetal_end_tangent(p1, p2, p3, g12, g23) * g12;
        seg.p[1] = p1 + m1 / 3.0F;
        seg.p[2] = p2 - m2 / 3.0F;
        set_attributes(seg, pts[i], pts[(i + 1) % n], static_cast<std::uint32_t>(i));
        out.push_back(seg);
    }
}

void build_bezier(const Spline& s, std::vector<Segment>& out) {
    const auto& pts = s.points;
    const std::size_t n = pts.size();
    const std::size_t count = s.closed ? n / 3 : (n - 1) / 3;
    out.reserve(count);
    for (std::size_t i = 0; i < count; ++i) {
        const std::size_t a = 3 * i;
        const std::size_t b = (a + 3) % n; // closed: the last segment ends at point 0
        Segment seg;
        seg.p[0] = pts[a].position;
        seg.p[1] = pts[a + 1].position;
        seg.p[2] = pts[a + 2].position;
        seg.p[3] = pts[b].position;
        set_attributes(seg, pts[a], pts[b], static_cast<std::uint32_t>(a));
        out.push_back(seg);
    }
}

void build_polyline(const Spline& s, std::vector<Segment>& out) {
    const auto& pts = s.points;
    const std::size_t n = pts.size();
    const std::size_t count = s.closed ? n : n - 1;
    out.reserve(count);
    for (std::size_t i = 0; i < count; ++i) {
        const glm::vec3 a = pts[i].position;
        const glm::vec3 b = pts[(i + 1) % n].position;
        Segment seg;
        seg.p[0] = a;
        seg.p[1] = a + (b - a) / 3.0F; // uniform speed: u is the chord fraction
        seg.p[2] = b - (b - a) / 3.0F;
        seg.p[3] = b;
        set_attributes(seg, pts[i], pts[(i + 1) % n], static_cast<std::uint32_t>(i));
        out.push_back(seg);
    }
}

void hash_bytes(std::uint64_t& h, const void* data, std::size_t size) {
    const auto* bytes = static_cast<const unsigned char*>(data);
    for (std::size_t i = 0; i < size; ++i) {
        h ^= bytes[i];
        h *= 1099511628211ULL; // FNV-1a 64 prime
    }
}

void hash_float(std::uint64_t& h, float f) {
    std::uint32_t bits = 0;
    std::memcpy(&bits, &f, sizeof bits);
    hash_bytes(h, &bits, sizeof bits);
}

void hash_u32(std::uint64_t& h, std::uint32_t v) { hash_bytes(h, &v, sizeof v); }

} // namespace

std::string_view kind_name(SplineKind kind) {
    for (const auto& k : k_kinds) {
        if (k.kind == kind) return k.name;
    }
    return "centripetal_catmull_rom";
}

bool parse_kind(std::string_view name, SplineKind& out) {
    for (const auto& k : k_kinds) {
        if (k.name == name) {
            out = k.kind;
            return true;
        }
    }
    return false;
}

std::expected<void, core::Error> validate(const Spline& s) {
    const std::size_t n = s.points.size();
    if (n > k_max_points) {
        return std::unexpected(core::make_error("points: at most " + std::to_string(k_max_points) +
                                                " points, got " + std::to_string(n)));
    }
    const std::size_t need = min_points(s);
    if (n < need) {
        return std::unexpected(core::make_error(
            "points: a " + std::string(s.closed ? "closed " : "") + std::string(kind_name(s.kind)) +
            " spline needs at least " + std::to_string(need) + " points, got " + std::to_string(n)));
    }
    if (s.kind == SplineKind::cubic_bezier) {
        const bool ok = s.closed ? n % 3 == 0 : n % 3 == 1;
        if (!ok) {
            return std::unexpected(core::make_error(
                std::string("points: a ") + (s.closed ? "closed cubic_bezier spline needs 3k points" :
                                                        "cubic_bezier spline needs 3k + 1 points") +
                " (anchor, handle, handle, anchor, ...), got " + std::to_string(n)));
        }
    }
    for (std::size_t i = 0; i < n; ++i) {
        const SplinePoint& p = s.points[i];
        if (!finite_in(p.position, k_max_coordinate)) {
            return std::unexpected(core::make_error(point_field(i, "position") +
                                                    ": expected finite coordinates within +-1e6 m"));
        }
        if (!std::isfinite(p.width) || p.width < 0.0F || p.width > k_max_width) {
            return std::unexpected(core::make_error(point_field(i, "width") + ": expected a number in [0, 10000]"));
        }
        if (!std::isfinite(p.height) || std::abs(p.height) > k_max_height) {
            return std::unexpected(
                core::make_error(point_field(i, "height") + ": expected a number in [-10000, 10000]"));
        }
    }
    if (!std::isfinite(s.ground_offset) || std::abs(s.ground_offset) > k_max_ground_offset) {
        return std::unexpected(core::make_error("ground_offset: expected a number in [-10000, 10000]"));
    }
    return {};
}

std::size_t segment_count(const Spline& s) {
    const std::size_t n = s.points.size();
    if (n < min_points(s)) return 0;
    switch (s.kind) {
    case SplineKind::cubic_bezier: return s.closed ? n / 3 : (n - 1) / 3;
    case SplineKind::centripetal_catmull_rom:
    case SplineKind::polyline: break;
    }
    return s.closed ? n : n - 1;
}

std::vector<Segment> build_segments(const Spline& s) {
    std::vector<Segment> out;
    if (!validate(s)) return out;
    switch (s.kind) {
    case SplineKind::centripetal_catmull_rom: build_catmull_rom(s, out); break;
    case SplineKind::cubic_bezier: build_bezier(s, out); break;
    case SplineKind::polyline: build_polyline(s, out); break;
    }
    return out;
}

glm::vec3 bezier_point(const glm::vec3 (&p)[4], float u) {
    // de Casteljau: stable, exact at u = 0 and 1, and exact on any axis
    // where the control points agree (a flat curve stays at its height).
    const glm::vec3 a = p[0] + (p[1] - p[0]) * u;
    const glm::vec3 b = p[1] + (p[2] - p[1]) * u;
    const glm::vec3 c = p[2] + (p[3] - p[2]) * u;
    const glm::vec3 d = a + (b - a) * u;
    const glm::vec3 e = b + (c - b) * u;
    return d + (e - d) * u;
}

glm::vec3 bezier_derivative(const glm::vec3 (&p)[4], float u) {
    const float v = 1.0F - u;
    return ((p[1] - p[0]) * (v * v) + (p[2] - p[1]) * (2.0F * v * u) + (p[3] - p[2]) * (u * u)) * 3.0F;
}

glm::vec3 bezier_second_derivative(const glm::vec3 (&p)[4], float u) {
    const float v = 1.0F - u;
    return ((p[2] - p[1] * 2.0F + p[0]) * v + (p[3] - p[2] * 2.0F + p[1]) * u) * 6.0F;
}

void bezier_split(const glm::vec3 (&p)[4], float u, glm::vec3 (&left)[4], glm::vec3 (&right)[4]) {
    const glm::vec3 a = p[0] + (p[1] - p[0]) * u;
    const glm::vec3 b = p[1] + (p[2] - p[1]) * u;
    const glm::vec3 c = p[2] + (p[3] - p[2]) * u;
    const glm::vec3 d = a + (b - a) * u;
    const glm::vec3 e = b + (c - b) * u;
    const glm::vec3 f = d + (e - d) * u;
    left[0] = p[0];
    left[1] = a;
    left[2] = d;
    left[3] = f;
    right[0] = f;
    right[1] = e;
    right[2] = c;
    right[3] = p[3];
}

std::uint64_t content_hash(const Spline& s, std::uint64_t seed) {
    std::uint64_t h = 14695981039346656037ULL ^ seed; // FNV-1a 64 offset basis
    hash_u32(h, static_cast<std::uint32_t>(s.kind));
    hash_u32(h, (s.closed ? 1U : 0U) | (s.follow_ground ? 2U : 0U));
    hash_float(h, s.ground_offset);
    hash_u32(h, s.network);
    hash_u32(h, s.id);
    hash_u32(h, static_cast<std::uint32_t>(s.points.size()));
    for (const SplinePoint& p : s.points) {
        hash_float(h, p.position.x);
        hash_float(h, p.position.y);
        hash_float(h, p.position.z);
        hash_float(h, p.width);
        hash_float(h, p.height);
        hash_u32(h, p.tags);
    }
    return h;
}

} // namespace engine::spline
