#include <algorithm>
#include <cmath>
#include <format>

#include "engine/geometry/mesh_ops.hpp"
#include "engine/geometry/straight_skeleton.hpp"
#include "engine/geometry/triangulate.hpp"

// Roof surfaces from a weighted straight skeleton (Kelly & Wonka 2011): each
// input edge's face is planar, so it is triangulated in its own plane
// coordinates (s along the eave, t = skeleton time) and lifted to 3D.

namespace engine::geometry {

namespace {

constexpr std::uint32_t kNoNode = 0xFFFFFFFFU;

// A face point: plan position, skeleton time, and the nodes it lies
// between (b == kNoNode for a node itself), so points cut on a shared arc
// are computed the same way from both faces (no cracks).
struct FacePoint {
    glm::dvec2 plan{0.0};
    double t = 0.0;
    std::uint32_t a = kNoNode;
    std::uint32_t b = kNoNode;
};

FacePoint node_point(const StraightSkeleton& sk, std::uint32_t n) {
    return FacePoint{sk.nodes[n].position, sk.nodes[n].time, n, kNoNode};
}

// The point at time `t` on segment p-q, interpolated from the endpoint with
// the smaller node id.
FacePoint cut(const FacePoint& p, const FacePoint& q, double t) {
    const bool swap = q.a < p.a;
    const FacePoint& lo = swap ? q : p;
    const FacePoint& hi = swap ? p : q;
    const double u = (t - lo.t) / (hi.t - lo.t);
    return FacePoint{lo.plan + (hi.plan - lo.plan) * u, t, std::min(p.a, q.a), std::max(p.a, q.a)};
}

// Sutherland-Hodgman against t <= limit (below = true) or t >= limit.
std::vector<FacePoint> clip(const std::vector<FacePoint>& poly, double limit, bool below) {
    std::vector<FacePoint> out;
    auto inside = [&](const FacePoint& p) { return below ? p.t <= limit : p.t >= limit; };
    for (std::size_t i = 0; i < poly.size(); ++i) {
        const FacePoint& p = poly[i];
        const FacePoint& q = poly[(i + 1) % poly.size()];
        if (inside(p)) {
            out.push_back(p);
        }
        if (inside(p) != inside(q) && p.t != q.t) {
            out.push_back(cut(p, q, limit));
        }
    }
    return out;
}

struct FacePlane {
    glm::dvec2 origin{0.0};
    glm::dvec2 dir{1.0, 0.0};
    glm::dvec3 normal{0.0, 1.0, 0.0}; // world, outward
    double slope_length = 1.0;        // metres up the face per unit time
};

void emit(MeshData& mesh, std::vector<std::uint32_t>& indices, const std::vector<FacePoint>& poly,
          const FacePlane& plane, const RoofOptions& options) {
    if (poly.size() < 3) {
        return;
    }
    Polygon flat;
    for (const FacePoint& p : poly) {
        flat.outer.push_back({glm::dot(p.plan - plane.origin, plane.dir), p.t * plane.slope_length});
    }
    // Faces are counter-clockwise in (s, up); slivers (zero area) are skipped.
    if (signed_area(flat.outer) <= 0.0) {
        return;
    }
    auto tri = triangulate(flat);
    if (!tri) {
        return; // a numerically degenerate sliver face
    }
    const auto base = static_cast<std::uint32_t>(mesh.vertices.size());
    for (std::size_t i = 0; i < poly.size(); ++i) {
        asset::Vertex v;
        v.position = glm::vec3(static_cast<float>(poly[i].plan.x),
                               static_cast<float>(options.base_height + poly[i].t * options.height_scale),
                               static_cast<float>(poly[i].plan.y));
        v.normal = glm::vec3(plane.normal);
        v.uv = glm::vec2(flat.outer[i] / options.uv_scale);
        mesh.vertices.push_back(v);
    }
    for (std::size_t k = 0; k + 2 < tri->indices.size(); k += 3) {
        const std::uint32_t a = base + tri->indices[k];
        std::uint32_t b = base + tri->indices[k + 1];
        std::uint32_t c = base + tri->indices[k + 2];
        const glm::dvec3 pa(mesh.vertices[a].position);
        const glm::dvec3 n = glm::cross(glm::dvec3(mesh.vertices[b].position) - pa,
                                        glm::dvec3(mesh.vertices[c].position) - pa);
        if (glm::dot(n, plane.normal) < 0.0) {
            std::swap(b, c);
        }
        indices.insert(indices.end(), {a, b, c});
    }
}

} // namespace

std::expected<MeshData, core::Error> roof_mesh(const StraightSkeleton& sk, const RoofOptions& options) {
    if (!std::isfinite(options.base_height) || !std::isfinite(options.height_scale) || options.height_scale <= 0.0 ||
        !std::isfinite(options.uv_scale) || options.uv_scale <= 0.0 || !std::isfinite(options.max_height) ||
        options.max_height < 0.0) {
        return std::unexpected(core::make_error(
            "roof: base_height must be finite, height_scale and uv_scale finite and > 0, max_height finite and >= 0"));
    }
    if (sk.faces.size() != sk.edge_weights.size()) {
        return std::unexpected(core::make_error("roof: skeleton has no faces (not from straight_skeleton?)"));
    }
    const double hs = options.height_scale;
    const double cut_time = options.max_height > 0.0 ? options.max_height / hs : 0.0;
    const bool flat_top = cut_time > 0.0 && cut_time < sk.max_time;

    MeshData mesh;
    std::vector<std::uint32_t> sloped;
    std::vector<std::uint32_t> gables;
    std::vector<Ring> top_pieces;
    for (const SkeletonFace& face : sk.faces) {
        if (face.nodes.size() < 3) {
            continue;
        }
        const double w = sk.edge_weights[face.edge];
        FacePlane plane;
        plane.origin = sk.nodes[face.nodes[0]].position;
        plane.dir = glm::normalize(sk.nodes[face.nodes[1]].position - plane.origin);
        const glm::dvec2 inward{-plane.dir.y, plane.dir.x};
        // Outward normal: up the gradient's opposite, (-n * hs, w) in (plan, up).
        plane.normal = glm::normalize(glm::dvec3(-inward.x * hs, w, -inward.y * hs));
        plane.slope_length = std::sqrt(w * w + hs * hs);

        std::vector<FacePoint> poly;
        for (const std::uint32_t n : face.nodes) {
            poly.push_back(node_point(sk, n));
        }
        if (flat_top) {
            std::vector<FacePoint> above = clip(poly, cut_time, false);
            if (above.size() >= 3 && w > 0.0) {
                Ring ring;
                for (const FacePoint& p : above) {
                    ring.push_back(p.plan);
                }
                top_pieces.push_back(std::move(ring));
            }
            poly = clip(poly, cut_time, true);
        }
        emit(mesh, w > 0.0 ? sloped : gables, poly, plane, options);
    }
    if (flat_top && !top_pieces.empty()) {
        auto cap = repair(top_pieces, FillRule::non_zero);
        if (!cap) {
            return std::unexpected(cap.error());
        }
        for (const Polygon& polygon : *cap) {
            auto tri = triangulate(polygon);
            if (!tri) {
                return std::unexpected(core::make_error(std::format("roof: flat top: {}", tri.error().message)));
            }
            const auto base = static_cast<std::uint32_t>(mesh.vertices.size());
            for (const glm::dvec2 p : tri->points) {
                asset::Vertex v;
                v.position = glm::vec3(static_cast<float>(p.x), static_cast<float>(options.base_height + options.max_height),
                                       static_cast<float>(p.y));
                v.normal = {0.0F, 1.0F, 0.0F};
                v.uv = glm::vec2(static_cast<float>(p.x / options.uv_scale), static_cast<float>(-p.y / options.uv_scale));
                mesh.vertices.push_back(v);
            }
            // Plan (x, y) -> world (x, z): counter-clockwise in plan faces -Y,
            // so reverse every triangle to face up.
            for (std::size_t k = 0; k + 2 < tri->indices.size(); k += 3) {
                sloped.insert(sloped.end(), {base + tri->indices[k], base + tri->indices[k + 2], base + tri->indices[k + 1]});
            }
        }
    }
    if (!sloped.empty()) {
        mesh.submeshes.push_back(asset::SubMesh{0, static_cast<std::uint32_t>(sloped.size()), options.material});
        mesh.indices = std::move(sloped);
    }
    if (!gables.empty()) {
        mesh.submeshes.push_back(asset::SubMesh{static_cast<std::uint32_t>(mesh.indices.size()),
                                                static_cast<std::uint32_t>(gables.size()), options.gable_material});
        mesh.indices.insert(mesh.indices.end(), gables.begin(), gables.end());
    }
    if (auto t = compute_tangents(mesh); !t) {
        return std::unexpected(t.error());
    }
    return mesh;
}

} // namespace engine::geometry
