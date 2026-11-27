// Procedural mesh generators — see include/engine/scene/primitives.hpp.
// Primitive -> MeshRenderer resolution lives in primitive_resolve.cpp.

#include "engine/scene/primitives.hpp"

#include <algorithm>
#include <cmath>
#include <limits>
#include <vector>

#include <glm/gtc/constants.hpp>

namespace engine::scene {

namespace {

using asset::Vertex;

// One latitude row of a surface of revolution about +Y: a circle of `radius`
// at height `y`, whose outward normal has radial component normal.x and
// vertical component normal.y. `v` is the texture coordinate along the
// profile (0 at the top row, increasing downward).
struct ProfileRow {
    float     radius = 0.0F;
    float     y = 0.0F;
    glm::vec2 normal{1.0F, 0.0F};
    float     v = 0.0F;
};

void finish(MeshData& d) {
    glm::vec3 lo(std::numeric_limits<float>::max());
    glm::vec3 hi(std::numeric_limits<float>::lowest());
    for (const Vertex& v : d.vertices) {
        lo = glm::min(lo, v.position);
        hi = glm::max(hi, v.position);
    }
    d.bounds = d.vertices.empty() ? asset::Aabb{} : asset::Aabb{lo, hi};
    d.submeshes = {asset::SubMesh{0, static_cast<std::uint32_t>(d.indices.size()), 0}};
}

// Appends a surface of revolution: (rows) x (sectors + 1) vertices, the seam
// column duplicated so U runs 0..1 without wrapping. U follows the angle
// theta (position = (r cos theta, y, r sin theta)), so the tangent is
// dP/dtheta and cross(N, T) points down the profile = +V. Rows of radius 0
// (poles, cone apex) are shifted half a sector so each one-triangle segment
// gets the tangent/normal of its own middle, and degenerate triangles are
// not emitted.
void append_revolution(MeshData& d, const std::vector<ProfileRow>& rows, std::uint32_t sectors) {
    const auto base = static_cast<std::uint32_t>(d.vertices.size());
    const std::uint32_t stride = sectors + 1;
    for (const ProfileRow& row : rows) {
        const float shift = row.radius == 0.0F ? 0.5F : 0.0F;
        for (std::uint32_t s = 0; s <= sectors; ++s) {
            // (A point row's last column is unused; keep its U in range.)
            const float u =
                std::min((static_cast<float>(s) + shift) / static_cast<float>(sectors), 1.0F);
            const float theta = u * glm::two_pi<float>();
            const float c = std::cos(theta);
            const float sn = std::sin(theta);
            Vertex vert;
            vert.position = {row.radius * c, row.y, row.radius * sn};
            vert.normal = glm::normalize(glm::vec3(row.normal.x * c, row.normal.y, row.normal.x * sn));
            vert.uv = {u, row.v};
            vert.tangent = glm::vec4(-sn, 0.0F, c, 1.0F);
            d.vertices.push_back(vert);
        }
    }
    for (std::size_t r = 0; r + 1 < rows.size(); ++r) {
        const bool top_point = rows[r].radius == 0.0F;
        const bool bottom_point = rows[r + 1].radius == 0.0F;
        if (top_point && bottom_point) continue;
        for (std::uint32_t s = 0; s < sectors; ++s) {
            const std::uint32_t i0 = base + static_cast<std::uint32_t>(r) * stride + s;
            const std::uint32_t i1 = i0 + 1;
            const std::uint32_t i2 = i0 + stride;
            const std::uint32_t i3 = i2 + 1;
            if (top_point) {
                for (std::uint32_t idx : {i0, i3, i2}) d.indices.push_back(idx);
            } else if (bottom_point) {
                for (std::uint32_t idx : {i0, i1, i2}) d.indices.push_back(idx);
            } else {
                for (std::uint32_t idx : {i0, i1, i2, i1, i3, i2}) d.indices.push_back(idx);
            }
        }
    }
}

// Appends a flat disc of `radius` at height `y` facing +Y (up) or -Y. Planar
// UVs centred on the disc; tangent +X, bitangent per the shared convention.
void append_disc(MeshData& d, float radius, float y, bool up, std::uint32_t sectors) {
    const auto centre = static_cast<std::uint32_t>(d.vertices.size());
    const glm::vec3 n(0.0F, up ? 1.0F : -1.0F, 0.0F);
    // cross(N, +X) is -Z for an upward face and +Z for a downward one: V runs
    // along that direction.
    const float v_sign = up ? -1.0F : 1.0F;
    const auto make = [&](float x, float z) {
        Vertex vert;
        vert.position = {x, y, z};
        vert.normal = n;
        vert.uv = {0.5F + x / (2.0F * radius), 0.5F + v_sign * z / (2.0F * radius)};
        vert.tangent = glm::vec4(1.0F, 0.0F, 0.0F, 1.0F);
        return vert;
    };
    d.vertices.push_back(make(0.0F, 0.0F));
    for (std::uint32_t s = 0; s <= sectors; ++s) {
        const float theta = static_cast<float>(s) / static_cast<float>(sectors) * glm::two_pi<float>();
        d.vertices.push_back(make(radius * std::cos(theta), radius * std::sin(theta)));
    }
    for (std::uint32_t s = 0; s < sectors; ++s) {
        const std::uint32_t a = centre + 1 + s;
        const std::uint32_t b = a + 1;
        // Counter-clockwise seen from the side the disc faces.
        if (up) {
            for (std::uint32_t idx : {centre, b, a}) d.indices.push_back(idx);
        } else {
            for (std::uint32_t idx : {centre, a, b}) d.indices.push_back(idx);
        }
    }
}

} // namespace

// A unit cube centred at the origin with per-face normals (24 vertices, 36
// indices).
MeshData make_cube_data() {
    MeshData d;
    const glm::vec3 face_normals[6] = {{1, 0, 0},  {-1, 0, 0}, {0, 1, 0},
                                       {0, -1, 0}, {0, 0, 1},  {0, 0, -1}};
    for (const glm::vec3& n : face_normals) {
        const glm::vec3 up = (std::abs(n.y) > 0.9F) ? glm::vec3(0, 0, 1) : glm::vec3(0, 1, 0);
        const glm::vec3 t = glm::normalize(glm::cross(up, n));
        const glm::vec3 b = glm::cross(n, t);
        const glm::vec3 c = n * 0.5F;
        const glm::vec3 corners[4] = {
            c - t * 0.5F - b * 0.5F, c + t * 0.5F - b * 0.5F,
            c + t * 0.5F + b * 0.5F, c - t * 0.5F + b * 0.5F};
        const glm::vec2 uvs[4] = {{0, 0}, {1, 0}, {1, 1}, {0, 1}};
        const auto base = static_cast<std::uint32_t>(d.vertices.size());
        for (int i = 0; i < 4; ++i) {
            Vertex v;
            v.position = corners[i];
            v.normal = n;
            v.uv = uvs[i];
            v.tangent = glm::vec4(t, 1.0F);
            d.vertices.push_back(v);
        }
        for (std::uint32_t idx : {base, base + 1, base + 2, base, base + 2, base + 3}) {
            d.indices.push_back(idx);
        }
    }
    d.bounds = {{-0.5F, -0.5F, -0.5F}, {0.5F, 0.5F, 0.5F}};
    d.submeshes = {asset::SubMesh{0, static_cast<std::uint32_t>(d.indices.size()), 0}};
    return d;
}

// A box with the given half-extents (the unit cube scaled into geometry, so the
// mesh carries no Transform scale — keeps physics rotation extraction clean).
MeshData make_box_data(const glm::vec3& half_extents) {
    MeshData d = make_cube_data();
    const glm::vec3 s = half_extents / 0.5F;
    for (Vertex& v : d.vertices) {
        v.position *= s;
    }
    d.bounds = {-half_extents, half_extents};
    return d;
}

MeshData make_sphere_data(float radius, std::uint32_t rings, std::uint32_t sectors) {
    rings = std::max(rings, 2U);
    sectors = std::max(sectors, 3U);
    std::vector<ProfileRow> rows;
    rows.reserve(rings + 1);
    for (std::uint32_t r = 0; r <= rings; ++r) {
        const float v = static_cast<float>(r) / static_cast<float>(rings);
        const float phi = v * glm::pi<float>(); // 0..pi (north to south pole)
        // Exact zero radius at the poles marks them as point rows.
        const float ring = (r == 0 || r == rings) ? 0.0F : std::sin(phi);
        rows.push_back({radius * ring, radius * std::cos(phi), {ring, std::cos(phi)}, v});
    }
    MeshData d;
    append_revolution(d, rows, sectors);
    finish(d);
    return d;
}

MeshData make_plane_data(float size) {
    const float h = size * 0.5F;
    MeshData d;
    // U along +X; V along cross(+Y, +X) = -Z.
    const glm::vec3 corners[4] = {{-h, 0, h}, {h, 0, h}, {h, 0, -h}, {-h, 0, -h}};
    const glm::vec2 uvs[4] = {{0, 0}, {1, 0}, {1, 1}, {0, 1}};
    for (int i = 0; i < 4; ++i) {
        Vertex v;
        v.position = corners[i];
        v.normal = {0.0F, 1.0F, 0.0F};
        v.uv = uvs[i];
        v.tangent = glm::vec4(1.0F, 0.0F, 0.0F, 1.0F);
        d.vertices.push_back(v);
    }
    d.indices = {0, 1, 2, 0, 2, 3};
    finish(d);
    return d;
}

MeshData make_cylinder_data(float radius, float height, std::uint32_t sectors) {
    sectors = std::max(sectors, 3U);
    const float h = height * 0.5F;
    MeshData d;
    append_revolution(d, {{radius, h, {1.0F, 0.0F}, 0.0F}, {radius, -h, {1.0F, 0.0F}, 1.0F}},
                      sectors);
    append_disc(d, radius, h, /*up=*/true, sectors);
    append_disc(d, radius, -h, /*up=*/false, sectors);
    finish(d);
    return d;
}

MeshData make_cone_data(float radius, float height, std::uint32_t sectors) {
    sectors = std::max(sectors, 3U);
    const float h = height * 0.5F;
    // The slant runs from (radius, -h) up to (0, +h); its outward normal in
    // the (radial, y) plane is perpendicular: (height, radius) normalised.
    const glm::vec2 n = glm::normalize(glm::vec2(height, radius));
    MeshData d;
    append_revolution(d, {{0.0F, h, n, 0.0F}, {radius, -h, n, 1.0F}}, sectors);
    append_disc(d, radius, -h, /*up=*/false, sectors);
    finish(d);
    return d;
}

MeshData make_capsule_data(float radius, float cylinder_height, std::uint32_t hemisphere_rings,
                           std::uint32_t sectors) {
    hemisphere_rings = std::max(hemisphere_rings, 1U);
    sectors = std::max(sectors, 3U);
    const float c = cylinder_height * 0.5F;
    // V follows arc length along the profile so the texel density is even
    // across the caps and the straight section.
    const float total = glm::pi<float>() * radius + cylinder_height;
    std::vector<ProfileRow> rows;
    rows.reserve(2 * (hemisphere_rings + 1));
    for (int cap = 0; cap < 2; ++cap) {
        for (std::uint32_t k = 0; k <= hemisphere_rings; ++k) {
            const float t = static_cast<float>(k) / static_cast<float>(hemisphere_rings);
            const float phi = (static_cast<float>(cap) + t) * glm::half_pi<float>();
            const bool pole = (cap == 0 && k == 0) || (cap == 1 && k == hemisphere_rings);
            const float ring = pole ? 0.0F : std::sin(phi);
            const float centre = cap == 0 ? c : -c;
            const float arc = radius * phi + (cap == 1 ? cylinder_height : 0.0F);
            rows.push_back({radius * ring, centre + radius * std::cos(phi), {ring, std::cos(phi)},
                            arc / total});
        }
    }
    MeshData d;
    append_revolution(d, rows, sectors);
    finish(d);
    return d;
}

MeshData make_primitive_data(PrimitiveShape shape) {
    constexpr std::uint32_t sectors = 48;
    switch (shape) {
    case PrimitiveShape::cube: return make_cube_data();
    case PrimitiveShape::sphere: return make_sphere_data(0.5F, 24, sectors);
    case PrimitiveShape::plane: return make_plane_data(1.0F);
    case PrimitiveShape::cylinder: return make_cylinder_data(0.5F, 1.0F, sectors);
    case PrimitiveShape::cone: return make_cone_data(0.5F, 1.0F, sectors);
    case PrimitiveShape::capsule: return make_capsule_data(0.5F, 1.0F, 12, sectors);
    }
    return make_cube_data();
}

asset::Aabb primitive_bounds(PrimitiveShape shape) {
    switch (shape) {
    case PrimitiveShape::plane: return {{-0.5F, 0.0F, -0.5F}, {0.5F, 0.0F, 0.5F}};
    case PrimitiveShape::capsule: return {{-0.5F, -1.0F, -0.5F}, {0.5F, 1.0F, 0.5F}};
    case PrimitiveShape::cube:
    case PrimitiveShape::sphere:
    case PrimitiveShape::cylinder:
    case PrimitiveShape::cone: break;
    }
    return {glm::vec3(-0.5F), glm::vec3(0.5F)};
}

} // namespace engine::scene
