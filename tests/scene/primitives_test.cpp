// Procedural primitive meshes and Primitive hashing (CPU only).

#include <cmath>
#include <string>

#include <entt/entity/registry.hpp>
#include <glm/glm.hpp>
#include <gtest/gtest.h>

#include "engine/scene/components.hpp"
#include "engine/scene/primitives.hpp"

namespace {

namespace s = engine::scene;
using s::PrimitiveShape;

constexpr PrimitiveShape k_shapes[] = {PrimitiveShape::cube,     PrimitiveShape::sphere,
                                       PrimitiveShape::plane,    PrimitiveShape::cylinder,
                                       PrimitiveShape::cone,     PrimitiveShape::capsule};

std::string shape_label(PrimitiveShape shape) {
    switch (shape) {
    case PrimitiveShape::cube: return "cube";
    case PrimitiveShape::sphere: return "sphere";
    case PrimitiveShape::plane: return "plane";
    case PrimitiveShape::cylinder: return "cylinder";
    case PrimitiveShape::cone: return "cone";
    case PrimitiveShape::capsule: return "capsule";
    }
    return "?";
}

// Structural checks every generated mesh must pass: valid indices, unit
// normals/tangents, a single submesh covering all indices, bounds that
// contain every vertex, and counter-clockwise (outward) winding — the face
// normal of each non-degenerate triangle agrees with its vertex normals.
void expect_well_formed(const s::MeshData& d, const std::string& label) {
    SCOPED_TRACE(label);
    ASSERT_FALSE(d.vertices.empty());
    ASSERT_EQ(d.indices.size() % 3, 0U);
    ASSERT_EQ(d.submeshes.size(), 1U);
    EXPECT_EQ(d.submeshes[0].index_count, d.indices.size());
    for (const std::uint32_t i : d.indices) ASSERT_LT(i, d.vertices.size());
    for (const auto& v : d.vertices) {
        EXPECT_NEAR(glm::length(v.normal), 1.0F, 1e-4F);
        EXPECT_NEAR(glm::length(glm::vec3(v.tangent)), 1.0F, 1e-4F);
        EXPECT_NEAR(glm::dot(v.normal, glm::vec3(v.tangent)), 0.0F, 1e-4F);
        EXPECT_TRUE(v.tangent.w == 1.0F || v.tangent.w == -1.0F);
        EXPECT_TRUE(glm::all(glm::greaterThanEqual(v.position, d.bounds.min - 1e-5F)));
        EXPECT_TRUE(glm::all(glm::lessThanEqual(v.position, d.bounds.max + 1e-5F)));
        EXPECT_TRUE(v.uv.x >= -1e-5F && v.uv.x <= 1.0F + 1e-5F && v.uv.y >= -1e-5F &&
                    v.uv.y <= 1.0F + 1e-5F);
    }
    int degenerate = 0;
    for (std::size_t t = 0; t < d.indices.size(); t += 3) {
        const auto& a = d.vertices[d.indices[t]];
        const auto& b = d.vertices[d.indices[t + 1]];
        const auto& c = d.vertices[d.indices[t + 2]];
        const glm::vec3 face = glm::cross(b.position - a.position, c.position - a.position);
        if (glm::length(face) < 1e-9F) {
            ++degenerate;
            continue;
        }
        EXPECT_GT(glm::dot(face, a.normal + b.normal + c.normal), 0.0F) << "triangle " << t / 3;
    }
    EXPECT_EQ(degenerate, 0);
}

TEST(Primitives, EveryShapeIsWellFormed) {
    for (const PrimitiveShape shape : k_shapes) {
        expect_well_formed(s::make_primitive_data(shape), shape_label(shape));
    }
    expect_well_formed(s::make_box_data(glm::vec3(1.0F, 2.0F, 3.0F)), "box");
    expect_well_formed(s::make_sphere_data(2.0F, 7, 9), "coarse sphere");
}

TEST(Primitives, UnitSizesMatchTheComponentContract) {
    for (const PrimitiveShape shape : k_shapes) {
        SCOPED_TRACE(shape_label(shape));
        const s::MeshData d = s::make_primitive_data(shape);
        const engine::asset::Aabb expected = s::primitive_bounds(shape);
        for (int i = 0; i < 3; ++i) {
            EXPECT_NEAR(d.bounds.min[i], expected.min[i], 1e-4F);
            EXPECT_NEAR(d.bounds.max[i], expected.max[i], 1e-4F);
        }
    }
    const engine::asset::Aabb capsule = s::primitive_bounds(PrimitiveShape::capsule);
    EXPECT_FLOAT_EQ(capsule.max.y - capsule.min.y, 2.0F);
    const engine::asset::Aabb plane = s::primitive_bounds(PrimitiveShape::plane);
    EXPECT_FLOAT_EQ(plane.max.y, 0.0F);
}

TEST(Primitives, VertexAndIndexCounts) {
    EXPECT_EQ(s::make_cube_data().vertices.size(), 24U);
    EXPECT_EQ(s::make_cube_data().indices.size(), 36U);
    EXPECT_EQ(s::make_plane_data(1.0F).vertices.size(), 4U);
    EXPECT_EQ(s::make_plane_data(1.0F).indices.size(), 6U);

    // Sphere: (rings + 1) x (sectors + 1) grid; quads between interior rows,
    // single triangles in the two polar bands.
    const s::MeshData sphere = s::make_sphere_data(1.0F, 8, 12);
    EXPECT_EQ(sphere.vertices.size(), 9U * 13U);
    EXPECT_EQ(sphere.indices.size(), 12U * 6U * 6U + 2U * 12U * 3U);

    // Cylinder: 2-row side + two caps (centre + sectors + 1 ring vertices).
    const s::MeshData cyl = s::make_cylinder_data(0.5F, 1.0F, 16);
    EXPECT_EQ(cyl.vertices.size(), 2U * 17U + 2U * 18U);
    EXPECT_EQ(cyl.indices.size(), 16U * 6U + 2U * 16U * 3U);

    const s::MeshData cone = s::make_cone_data(0.5F, 1.0F, 16);
    EXPECT_EQ(cone.vertices.size(), 2U * 17U + 18U);
    EXPECT_EQ(cone.indices.size(), 16U * 3U + 16U * 3U);

    // Capsule: two hemispheres of (rings + 1) rows each; the band between the
    // two equator rows is the cylinder.
    const s::MeshData cap = s::make_capsule_data(0.5F, 1.0F, 4, 10);
    EXPECT_EQ(cap.vertices.size(), 10U * 11U);
    EXPECT_EQ(cap.indices.size(), 7U * 10U * 6U + 2U * 10U * 3U);
}

TEST(Primitives, SideNormalsPointAwayFromTheAxis) {
    const s::MeshData cyl = s::make_cylinder_data(0.5F, 1.0F, 24);
    const s::MeshData cone = s::make_cone_data(0.5F, 1.0F, 24);
    for (const auto* d : {&cyl, &cone}) {
        for (const auto& v : d->vertices) {
            const glm::vec2 radial(v.position.x, v.position.z);
            if (glm::length(radial) > 0.25F && std::abs(v.normal.y) < 0.99F) {
                EXPECT_GT(glm::dot(glm::normalize(radial), glm::vec2(v.normal.x, v.normal.z)), 0.0F);
            }
        }
    }
    // The cone's slant normal tilts up by atan(radius / height).
    for (const auto& v : cone.vertices) {
        if (std::abs(v.normal.y) < 0.99F) EXPECT_NEAR(v.normal.y, 0.5F / std::sqrt(1.25F), 1e-4F);
    }
}

TEST(Primitives, HashTracksShapeAndMaterial) {
    s::Primitive a;
    s::Primitive b = a;
    EXPECT_EQ(s::primitive_hash(a), s::primitive_hash(b));
    EXPECT_NE(s::primitive_hash(a), 0U);
    b.resolved = 1234; // system-managed, not content
    EXPECT_EQ(s::primitive_hash(a), s::primitive_hash(b));
    b.shape = PrimitiveShape::sphere;
    EXPECT_NE(s::primitive_hash(a), s::primitive_hash(b));
    b = a;
    b.color.r = 0.5F;
    EXPECT_NE(s::primitive_hash(a), s::primitive_hash(b));
    b = a;
    b.emissive = glm::vec3(-0.0F); // -0 hashes like +0
    EXPECT_EQ(s::primitive_hash(a), s::primitive_hash(b));
}

TEST(Primitives, MaterialFlags) {
    s::Primitive p;
    EXPECT_EQ(s::primitive_material_params(p).flags, 0U);
    p.color.a = 0.5F;
    EXPECT_NE(s::primitive_material_params(p).flags & engine::asset::material_blend, 0U);
    p.color.a = 1.0F;
    p.shape = PrimitiveShape::plane;
    EXPECT_NE(s::primitive_material_params(p).flags & engine::asset::material_double_sided, 0U);
    p.metallic = 0.75F;
    p.emissive = {2.0F, 1.0F, 0.0F};
    const auto params = s::primitive_material_params(p);
    EXPECT_FLOAT_EQ(params.metallic_factor, 0.75F);
    EXPECT_FLOAT_EQ(params.emissive_factor.x, 2.0F);
}

TEST(Primitives, NeedResolveSeesStaleAndMissingRenderers) {
    entt::registry r;
    EXPECT_FALSE(s::primitives_need_resolve(r));
    const entt::entity e = r.create();
    s::Primitive& p = r.emplace<s::Primitive>(e);
    EXPECT_TRUE(s::primitives_need_resolve(r)); // resolved == 0
    p.resolved = s::primitive_hash(p);
    EXPECT_TRUE(s::primitives_need_resolve(r)); // no MeshRenderer yet
    r.emplace<s::MeshRenderer>(e);
    EXPECT_FALSE(s::primitives_need_resolve(r));
    r.get<s::Primitive>(e).roughness = 0.9F; // edited in place (inspector, snapshot)
    EXPECT_TRUE(s::primitives_need_resolve(r));
}

} // namespace
