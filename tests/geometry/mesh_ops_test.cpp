#include <algorithm>
#include <cmath>
#include <set>
#include <tuple>
#include <vector>

#include <glm/gtc/matrix_transform.hpp>
#include <gtest/gtest.h>

#include "engine/geometry/mesh_ops.hpp"
#include "geometry/geometry_test_util.hpp"
#include "support/expect_ok.hpp"

using namespace engine::geometry;
namespace asset = engine::asset;
using quat::test::geometry::signed_volume;

namespace {

// A unit cube with 8 shared corners (no normals yet).
MeshData shared_cube() {
    MeshData m;
    for (int i = 0; i < 8; ++i) {
        asset::Vertex v;
        v.position = {static_cast<float>(i & 1), static_cast<float>((i >> 1) & 1), static_cast<float>((i >> 2) & 1)};
        m.vertices.push_back(v);
    }
    m.indices = {0, 2, 1, 1, 2, 3, 4, 5, 6, 5, 7, 6, 0, 1, 4, 1, 5, 4,
                 2, 6, 3, 3, 6, 7, 0, 4, 2, 2, 4, 6, 1, 3, 5, 3, 7, 5};
    return m;
}

std::multiset<std::tuple<float, float, float>> triangle_set(const MeshData& m) {
    std::multiset<std::tuple<float, float, float>> s;
    for (std::size_t t = 0; t < m.triangle_count(); ++t) {
        glm::vec3 c{0.0F};
        for (int k = 0; k < 3; ++k) {
            c += m.vertices[m.indices[3 * t + k]].position;
        }
        s.insert({c.x, c.y, c.z});
    }
    return s;
}

} // namespace

TEST(GeometryMesh, ValidateCatchesBrokenMeshes) {
    MeshData m = shared_cube();
    EXPECT_OK(validate(m));
    MeshData bad = m;
    bad.indices.push_back(0);
    EXPECT_ERR(validate(bad));
    bad = m;
    bad.indices[5] = 99;
    EXPECT_ERR(validate(bad));
    bad = m;
    bad.vertices[3].position.x = NAN;
    EXPECT_ERR(validate(bad));
    bad = m;
    bad.submeshes = {{0, 12, 0}, {15, 24, 1}}; // gap
    EXPECT_ERR(validate(bad));
    bad.submeshes = {{0, 12, 0}};               // short
    EXPECT_ERR(validate(bad));
}

TEST(GeometryMesh, HashIsStableAndSensitive) {
    MeshData a = shared_cube();
    MeshData b = a;
    EXPECT_EQ(mesh_hash(a), mesh_hash(b));
    b.vertices[0].position.x = -0.0F; // -0 hashes as +0
    EXPECT_EQ(mesh_hash(a), mesh_hash(b));
    b.vertices[0].position.x = 1e-7F;
    EXPECT_NE(mesh_hash(a), mesh_hash(b));
    b = a;
    std::swap(b.indices[0], b.indices[1]);
    EXPECT_NE(mesh_hash(a), mesh_hash(b));
}

TEST(GeometryMesh, AppendTransformsAndKeepsMirroredWindingOutward) {
    const MeshData cube = shared_cube();
    MeshData out;
    append(out, cube, glm::mat4{1.0F});
    append(out, cube, glm::scale(glm::mat4{1.0F}, glm::vec3(-2.0F, 1.0F, 1.0F)));
    ASSERT_OK(validate(out));
    EXPECT_EQ(out.submeshes.size(), 2U);
    // Both cubes stay outward-facing (positive volume): 1 + 2.
    EXPECT_NEAR(signed_volume(out), 3.0, 1e-5);
}

TEST(GeometryMesh, WeldMergesAndDropsDegenerates) {
    MeshData m = shared_cube();
    MeshData split;
    for (const std::uint32_t i : m.indices) { // unindexed copy
        split.vertices.push_back(m.vertices[i]);
        split.indices.push_back(static_cast<std::uint32_t>(split.indices.size()));
    }
    split.vertices[0].position += glm::vec3(1e-5F);
    weld(split);
    EXPECT_EQ(split.vertices.size(), 9U); // the nudged corner stays apart
    WeldOptions tolerant;
    tolerant.position_tolerance = 1e-3F;
    weld(split, tolerant);
    EXPECT_EQ(split.vertices.size(), 8U);
    EXPECT_EQ(split.triangle_count(), 12U);
    split.indices.insert(split.indices.end(), {0, 0, 1});
    weld(split);
    EXPECT_EQ(split.triangle_count(), 12U);
}

TEST(GeometryMesh, CreaseNormals) {
    MeshData flat = shared_cube();
    compute_normals(flat, NormalOptions{30.0F});
    EXPECT_EQ(flat.vertices.size(), 24U); // every corner split per face
    for (std::size_t t = 0; t < flat.triangle_count(); ++t) {
        const glm::dvec3 face = quat::test::geometry::triangle_normal(flat, t);
        for (int k = 0; k < 3; ++k) {
            EXPECT_NEAR(glm::dot(glm::dvec3(flat.vertices[flat.indices[3 * t + k]].normal), face), 1.0, 1e-6);
        }
    }
    MeshData smooth = shared_cube();
    compute_normals(smooth, NormalOptions{180.0F});
    EXPECT_EQ(smooth.vertices.size(), 8U);
    for (const asset::Vertex& v : smooth.vertices) {
        const glm::vec3 diagonal = glm::normalize(v.position - glm::vec3(0.5F));
        EXPECT_NEAR(glm::dot(v.normal, diagonal), 1.0F, 1e-5F); // corner angles are equal
    }
}

TEST(GeometryMesh, BoxUvsAndTangents) {
    MeshData m = shared_cube();
    compute_normals(m, NormalOptions{30.0F});
    UvOptions uv;
    uv.scale = 0.5F;
    generate_uvs(m, uv);
    for (const asset::Vertex& v : m.vertices) {
        EXPECT_GE(v.uv.x, -2.0F);
        EXPECT_LE(v.uv.x, 2.0F);
        // One axis of the face is constant; the UV spans the other two
        // (|uv| in {0, 2} with scale 0.5).
        EXPECT_TRUE(std::fabs(v.uv.y) == 0.0F || std::fabs(v.uv.y) == 2.0F);
    }
    ASSERT_OK(compute_tangents(m));
    for (const asset::Vertex& v : m.vertices) {
        const glm::vec3 t(v.tangent);
        EXPECT_NEAR(glm::length(t), 1.0F, 1e-5F);
        EXPECT_NEAR(glm::dot(t, v.normal), 0.0F, 1e-5F);
        EXPECT_EQ(std::fabs(v.tangent.w), 1.0F);
        // The box projection's frames are right-handed: w = +1.
        EXPECT_EQ(v.tangent.w, 1.0F);
    }
    ASSERT_OK(validate(m));
}

TEST(GeometryMesh, OptimizeKeepsTheTriangles) {
    MeshData m = shared_cube();
    compute_normals(m, NormalOptions{30.0F});
    m.submeshes = {{0, 18, 0}, {18, 18, 1}};
    const auto before = triangle_set(m);
    const double volume = signed_volume(m);
    optimize(m);
    ASSERT_OK(validate(m));
    EXPECT_EQ(triangle_set(m), before);
    EXPECT_NEAR(signed_volume(m), volume, 1e-6);
    // Fetch order: first use order.
    std::uint32_t next = 0;
    for (const std::uint32_t i : m.indices) {
        EXPECT_LE(i, next);
        next = std::max(next, i + 1);
    }
}
