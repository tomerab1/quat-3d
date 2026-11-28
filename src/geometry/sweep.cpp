#include <cmath>
#include <format>

#include "engine/geometry/mesh_ops.hpp"
#include "engine/geometry/sweep.hpp"
#include "engine/geometry/triangulate.hpp"

namespace engine::geometry {

namespace {

// A profile point placed on a frame, stretched across the mitre.
glm::dvec3 place(const PathFrame& f, glm::dvec2 p) {
    glm::dvec3 q = f.normal * p.x + f.binormal * p.y;
    if (f.miter_scale != 1.0) {
        q += f.miter_direction * (glm::dot(q, f.miter_direction) * (f.miter_scale - 1.0));
    }
    return f.origin + q;
}

asset::Vertex vertex(glm::dvec3 p, glm::dvec2 uv) {
    asset::Vertex v;
    v.position = glm::vec3(p);
    v.uv = glm::vec2(uv);
    return v;
}

std::expected<void, core::Error> check_profile(std::span<const glm::dvec2> profile, std::size_t index) {
    for (std::size_t j = 0; j < profile.size(); ++j) {
        if (!std::isfinite(profile[j].x) || !std::isfinite(profile[j].y)) {
            return std::unexpected(core::make_error(std::format("profile {} point {} is not finite", index, j)));
        }
    }
    return {};
}

void add_cap(MeshData& mesh, std::vector<std::uint32_t>& out, const PathFrame& frame, bool facing_forward, double uv_scale, const Triangulation& tri) {
    const auto base = static_cast<std::uint32_t>(mesh.vertices.size());
    for (const glm::dvec2 p : tri.points) {
        asset::Vertex v = vertex(place(frame, p), p / uv_scale);
        v.normal = glm::vec3(facing_forward ? frame.tangent : -frame.tangent);
        mesh.vertices.push_back(v);
    }
    // Counter-clockwise in (normal, binormal) faces +tangent.
    for (std::size_t k = 0; k + 2 < tri.indices.size(); k += 3) {
        const std::uint32_t a = base + tri.indices[k];
        const std::uint32_t b = base + tri.indices[k + 1];
        const std::uint32_t c = base + tri.indices[k + 2];
        if (facing_forward) {
            out.insert(out.end(), {a, b, c});
        } else {
            out.insert(out.end(), {a, c, b});
        }
    }
}

} // namespace

std::expected<MeshData, core::Error> loft(std::span<const std::vector<glm::dvec2>> profiles,
                                          std::span<const PathFrame> frames, const SweepOptions& options) {
    if (profiles.size() != frames.size() || frames.size() < 2) {
        return std::unexpected(core::make_error(std::format(
            "loft: {} profiles for {} frames; needs one profile per frame and at least 2", profiles.size(),
            frames.size())));
    }
    const std::size_t count = profiles[0].size();
    if (count < 2 || (options.closed_profile && count < 3)) {
        return std::unexpected(core::make_error(std::format("loft: profile has {} points", count)));
    }
    for (std::size_t i = 0; i < profiles.size(); ++i) {
        if (profiles[i].size() != count) {
            return std::unexpected(core::make_error(std::format(
                "loft: profile {} has {} points, profile 0 has {}", i, profiles[i].size(), count)));
        }
        if (auto r = check_profile(profiles[i], i); !r) {
            return std::unexpected(r.error());
        }
        if (!(frames[i].miter_scale <= options.miter_limit)) {
            return std::unexpected(core::make_error(std::format(
                "loft: the corner at path point {} stretches the profile {:.2f}x, over the miter limit {:.2f}; "
                "round the corner or raise the limit",
                i, frames[i].miter_scale, options.miter_limit)));
        }
    }
    if (!(options.uv_scale > 0.0) || !std::isfinite(options.uv_scale)) {
        return std::unexpected(core::make_error("loft: uv_scale must be finite and > 0"));
    }
    const bool caps = options.closed_profile && !options.closed_path && (options.cap_start || options.cap_end);

    MeshData mesh;
    std::vector<std::uint32_t> sides;
    // Rings; the seam column / row is duplicated so UVs can run on.
    const std::size_t columns = options.closed_profile ? count + 1 : count;
    const std::size_t rows = options.closed_path ? frames.size() + 1 : frames.size();
    const double path_length =
        options.closed_path ? frames.back().distance + glm::length(frames[0].origin - frames.back().origin) : 0.0;
    for (std::size_t i = 0; i < rows; ++i) {
        const std::size_t fi = i % frames.size();
        const PathFrame& f = frames[fi];
        const double u = (i == frames.size() ? path_length : f.distance) / options.uv_scale;
        double v = 0.0;
        for (std::size_t j = 0; j < columns; ++j) {
            const glm::dvec2 p = profiles[fi][j % count];
            if (j > 0) {
                v += glm::length(p - profiles[fi][j - 1]) / options.uv_scale;
            }
            mesh.vertices.push_back(vertex(place(f, p), {u, v}));
        }
    }
    for (std::size_t i = 0; i + 1 < rows; ++i) {
        for (std::size_t j = 0; j + 1 < columns; ++j) {
            const auto a = static_cast<std::uint32_t>(i * columns + j);
            const auto b = static_cast<std::uint32_t>(i * columns + j + 1);
            const auto c = static_cast<std::uint32_t>((i + 1) * columns + j + 1);
            const auto d = static_cast<std::uint32_t>((i + 1) * columns + j);
            sides.insert(sides.end(), {a, b, d, b, c, d});
        }
    }
    mesh.indices = std::move(sides);
    mesh.submeshes.push_back(asset::SubMesh{0, static_cast<std::uint32_t>(mesh.indices.size()), options.material});
    NormalOptions normals;
    normals.crease_angle_degrees = options.crease_angle_degrees;
    compute_normals(mesh, normals);

    if (caps) {
        std::vector<std::uint32_t> cap_indices;
        for (const bool end : {false, true}) {
            if (end ? !options.cap_end : !options.cap_start) {
                continue;
            }
            const std::size_t fi = end ? frames.size() - 1 : 0;
            Polygon outline;
            outline.outer.assign(profiles[fi].begin(), profiles[fi].end());
            if (signed_area(outline.outer) < 0.0) {
                return std::unexpected(core::make_error(
                    "loft: capped profiles must be counter-clockwise (outward faces on the right of the profile)"));
            }
            auto tri = triangulate(outline);
            if (!tri) {
                return std::unexpected(core::make_error(std::format("loft: cap: {}", tri.error().message)));
            }
            add_cap(mesh, cap_indices, frames[fi], end, options.uv_scale, *tri);
        }
        mesh.submeshes.push_back(asset::SubMesh{static_cast<std::uint32_t>(mesh.indices.size()),
                                                static_cast<std::uint32_t>(cap_indices.size()), options.cap_material});
        mesh.indices.insert(mesh.indices.end(), cap_indices.begin(), cap_indices.end());
    }
    if (auto t = compute_tangents(mesh); !t) {
        return std::unexpected(t.error());
    }
    if (auto valid = validate(mesh); !valid) {
        return std::unexpected(valid.error());
    }
    return mesh;
}

std::expected<MeshData, core::Error> sweep(std::span<const glm::dvec2> profile, std::span<const PathFrame> frames,
                                           const SweepOptions& options) {
    const std::vector<std::vector<glm::dvec2>> profiles(frames.size(),
                                                        std::vector<glm::dvec2>(profile.begin(), profile.end()));
    return loft(profiles, frames, options);
}

} // namespace engine::geometry
