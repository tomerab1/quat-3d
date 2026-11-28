#include <cmath>
#include <format>

#include "engine/geometry/mesh_ops.hpp"
#include "engine/geometry/straight_skeleton.hpp"
#include "engine/geometry/sweep.hpp"
#include "engine/geometry/triangulate.hpp"

namespace engine::geometry {

namespace {

// The mitred inset of a ring by `d` (positive: towards the interior, which
// is on the left of every ring): each vertex moves along its bisector by
// d / cos(half angle), so every edge moves exactly d (the straight
// skeleton's wavefront before its first event).
Ring inset(const Ring& ring, double d) {
    Ring out(ring.size());
    const std::size_t n = ring.size();
    for (std::size_t i = 0; i < n; ++i) {
        const glm::dvec2 a = glm::normalize(ring[i] - ring[(i + n - 1) % n]);
        const glm::dvec2 b = glm::normalize(ring[(i + 1) % n] - ring[i]);
        const glm::dvec2 na{-a.y, a.x};
        const glm::dvec2 nb{-b.y, b.x};
        out[i] = ring[i] + (na + nb) * (d / (1.0 + glm::dot(na, nb)));
    }
    return out;
}

glm::vec3 world(glm::dvec2 p, double h) {
    return {static_cast<float>(p.x), static_cast<float>(h), static_cast<float>(p.y)};
}

void push_facing(MeshData& mesh, std::vector<std::uint32_t>& out, std::uint32_t a, std::uint32_t b,
                 std::uint32_t c, glm::dvec3 facing) {
    const glm::dvec3 pa(mesh.vertices[a].position);
    const glm::dvec3 n =
        glm::cross(glm::dvec3(mesh.vertices[b].position) - pa, glm::dvec3(mesh.vertices[c].position) - pa);
    if (glm::dot(n, facing) < 0.0) {
        std::swap(b, c);
    }
    out.insert(out.end(), {a, b, c});
}

} // namespace

std::expected<MeshData, core::Error> extrude(const Polygon& polygon, const ExtrudeOptions& options) {
    if (!std::isfinite(options.height) || options.height <= 0.0 || !std::isfinite(options.base_height) ||
        !std::isfinite(options.taper) || !std::isfinite(options.uv_scale) || options.uv_scale <= 0.0) {
        return std::unexpected(core::make_error(
            "extrude: height and uv_scale must be finite and > 0, base_height and taper finite"));
    }
    if (polygon.outer.size() < 3 || signed_area(polygon.outer) <= 0.0) {
        return std::unexpected(core::make_error(
            "extrude: the outer ring needs 3+ points, counter-clockwise (geometry::repair returns that)"));
    }
    if (options.taper > 0.0) {
        // An inset reaching the first skeleton event would fold the top.
        auto skeleton = straight_skeleton(polygon);
        if (!skeleton) {
            return std::unexpected(skeleton.error());
        }
        double first_event = skeleton->max_time;
        for (std::size_t n = 0; n < skeleton->nodes.size(); ++n) {
            if (skeleton->nodes[n].time > 0.0) {
                first_event = std::min(first_event, skeleton->nodes[n].time);
            }
        }
        if (options.taper >= first_event) {
            return std::unexpected(core::make_error(std::format(
                "extrude: taper {} m folds the top outline (its first straight-skeleton event is at {} m); use a "
                "smaller taper or a roof",
                options.taper, first_event)));
        }
    }
    std::vector<const Ring*> rings{&polygon.outer};
    for (const Ring& hole : polygon.holes) {
        if (hole.size() >= 3) {
            rings.push_back(&hole);
        }
    }
    Polygon top;
    for (std::size_t r = 0; r < rings.size(); ++r) {
        Ring ring = inset(*rings[r], options.taper);
        if (r == 0) {
            top.outer = std::move(ring);
        } else {
            top.holes.push_back(std::move(ring));
        }
    }
    if (options.taper < 0.0) {
        // Flaring out: reflex corners can make the outline cross itself.
        auto repaired = repair(std::vector<Ring>{top.outer});
        if (!repaired || repaired->size() != 1 ||
            std::fabs(area((*repaired)[0]) - std::fabs(signed_area(top.outer))) > 1e-6 * std::fabs(signed_area(top.outer))) {
            return std::unexpected(core::make_error(
                std::format("extrude: taper {} m makes the top outline cross itself", options.taper)));
        }
    }

    const double y0 = options.base_height;
    const double y1 = options.base_height + options.height;
    MeshData mesh;
    std::vector<std::uint32_t> sides;
    for (std::size_t r = 0; r < rings.size(); ++r) {
        const Ring& bottom = *rings[r];
        const Ring& upper = r == 0 ? top.outer : top.holes[r - 1];
        const std::size_t n = bottom.size();
        double u = 0.0;
        for (std::size_t i = 0; i < n; ++i) {
            const std::size_t j = (i + 1) % n;
            const glm::dvec2 edge = bottom[j] - bottom[i];
            const double len = glm::length(edge);
            const glm::dvec2 out_dir = glm::dvec2{edge.y, -edge.x} / len; // right of the edge: outside
            const double slant = std::sqrt(options.height * options.height + options.taper * options.taper);
            const auto base = static_cast<std::uint32_t>(mesh.vertices.size());
            const double v1 = slant / options.uv_scale;
            asset::Vertex v;
            v.position = world(bottom[i], y0);
            v.uv = glm::vec2(u / options.uv_scale, 0.0);
            mesh.vertices.push_back(v);
            v.position = world(bottom[j], y0);
            v.uv = glm::vec2((u + len) / options.uv_scale, 0.0);
            mesh.vertices.push_back(v);
            v.position = world(upper[j], y1);
            v.uv = glm::vec2((u + len) / options.uv_scale, v1);
            mesh.vertices.push_back(v);
            v.position = world(upper[i], y1);
            v.uv = glm::vec2(u / options.uv_scale, v1);
            mesh.vertices.push_back(v);
            const glm::dvec3 facing{out_dir.x * options.height, options.taper, out_dir.y * options.height};
            push_facing(mesh, sides, base, base + 1, base + 2, facing);
            push_facing(mesh, sides, base, base + 2, base + 3, facing);
            u += len;
        }
    }
    mesh.indices = std::move(sides);
    mesh.submeshes.push_back(
        asset::SubMesh{0, static_cast<std::uint32_t>(mesh.indices.size()), options.side_material});
    NormalOptions normals;
    normals.crease_angle_degrees = options.crease_angle_degrees;
    compute_normals(mesh, normals);

    std::vector<std::uint32_t> caps;
    for (const bool up : {false, true}) {
        if (up ? !options.cap_top : !options.cap_bottom) {
            continue;
        }
        auto tri = triangulate(up ? top : polygon);
        if (!tri) {
            return std::unexpected(core::make_error(std::format("extrude: cap: {}", tri.error().message)));
        }
        const auto base = static_cast<std::uint32_t>(mesh.vertices.size());
        for (const glm::dvec2 p : tri->points) {
            asset::Vertex v;
            v.position = world(p, up ? y1 : y0);
            v.normal = {0.0F, up ? 1.0F : -1.0F, 0.0F};
            v.uv = glm::vec2(p / options.uv_scale);
            mesh.vertices.push_back(v);
        }
        for (std::size_t k = 0; k + 2 < tri->indices.size(); k += 3) {
            push_facing(mesh, caps, base + tri->indices[k], base + tri->indices[k + 1], base + tri->indices[k + 2],
                        {0.0, up ? 1.0 : -1.0, 0.0});
        }
    }
    if (!caps.empty()) {
        mesh.submeshes.push_back(asset::SubMesh{static_cast<std::uint32_t>(mesh.indices.size()),
                                                static_cast<std::uint32_t>(caps.size()), options.cap_material});
        mesh.indices.insert(mesh.indices.end(), caps.begin(), caps.end());
    }
    if (auto t = compute_tangents(mesh); !t) {
        return std::unexpected(t.error());
    }
    return mesh;
}

} // namespace engine::geometry
