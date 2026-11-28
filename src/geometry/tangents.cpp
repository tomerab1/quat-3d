#include "engine/geometry/mesh_ops.hpp"

#include <climits>
#include <format>

#include <mikktspace.h>

#include "geometry/mesh_detail.hpp"

// Tangents by MikkTSpace (Mikkelsen 2008), the convention glTF 2.0 and most
// bakers use. The vendored copy (vendor/mikktspace) is compiled under the
// kernel's floating-point rules; see mikktspace_impl.c.

namespace engine::geometry {

namespace {

struct MikkMesh {
    std::vector<asset::Vertex>* corners = nullptr; // one per index, written in place
};

asset::Vertex& corner(const SMikkTSpaceContext* ctx, int face, int vert) {
    auto* m = static_cast<MikkMesh*>(ctx->m_pUserData);
    return (*m->corners)[static_cast<std::size_t>(face) * 3 + static_cast<std::size_t>(vert)];
}

int num_faces(const SMikkTSpaceContext* ctx) {
    const auto* m = static_cast<const MikkMesh*>(ctx->m_pUserData);
    return static_cast<int>(m->corners->size() / 3);
}

int num_vertices_of_face(const SMikkTSpaceContext*, const int) { return 3; }

void get_position(const SMikkTSpaceContext* ctx, float out[], const int face, const int vert) {
    const glm::vec3 p = corner(ctx, face, vert).position;
    out[0] = p.x;
    out[1] = p.y;
    out[2] = p.z;
}

void get_normal(const SMikkTSpaceContext* ctx, float out[], const int face, const int vert) {
    const glm::vec3 n = corner(ctx, face, vert).normal;
    out[0] = n.x;
    out[1] = n.y;
    out[2] = n.z;
}

void get_tex_coord(const SMikkTSpaceContext* ctx, float out[], const int face, const int vert) {
    const glm::vec2 uv = corner(ctx, face, vert).uv;
    out[0] = uv.x;
    out[1] = uv.y;
}

void set_tspace_basic(const SMikkTSpaceContext* ctx, const float tangent[], const float sign, const int face,
                      const int vert) {
    corner(ctx, face, vert).tangent = glm::vec4(tangent[0], tangent[1], tangent[2], sign);
}

} // namespace

std::expected<void, core::Error> compute_tangents(MeshData& mesh) {
    if (mesh.indices.size() / 3 > static_cast<std::size_t>(INT_MAX / 3)) {
        return std::unexpected(core::make_error(
            std::format("tangents: {} triangles is more than MikkTSpace supports", mesh.indices.size() / 3)));
    }
    if (mesh.indices.empty()) {
        return {};
    }
    std::vector<asset::Vertex> corners = detail::corners_of(mesh);
    MikkMesh user{&corners};
    SMikkTSpaceInterface iface{};
    iface.m_getNumFaces = num_faces;
    iface.m_getNumVerticesOfFace = num_vertices_of_face;
    iface.m_getPosition = get_position;
    iface.m_getNormal = get_normal;
    iface.m_getTexCoord = get_tex_coord;
    iface.m_setTSpaceBasic = set_tspace_basic;
    SMikkTSpaceContext ctx{};
    ctx.m_pInterface = &iface;
    ctx.m_pUserData = &user;
    if (genTangSpaceDefault(&ctx) == 0) {
        return std::unexpected(core::make_error("tangents: MikkTSpace failed (out of memory)"));
    }
    detail::rebuild_from_corners(mesh, corners);
    return {};
}

} // namespace engine::geometry
