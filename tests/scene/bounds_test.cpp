// Draw bounds (issue #40): exact AABB transforms and what each drawable
// contributes.

#include <entt/entity/registry.hpp>
#include <glm/glm.hpp>
#include <glm/gtc/matrix_transform.hpp>
#include <gtest/gtest.h>

#include "engine/scene/bounds.hpp"
#include "engine/scene/components.hpp"

namespace {

namespace s = engine::scene;
using engine::asset::Aabb;

void expect_vec3(const glm::vec3& got, const glm::vec3& want) {
    EXPECT_NEAR(got.x, want.x, 1e-4F);
    EXPECT_NEAR(got.y, want.y, 1e-4F);
    EXPECT_NEAR(got.z, want.z, 1e-4F);
}

TEST(Bounds, TransformIsExactUnderRotationAndScale) {
    const Aabb unit{glm::vec3(-0.5F), glm::vec3(0.5F)};
    // Scale (2, 1, 4), then 90 degrees about Y: x and z swap extents.
    glm::mat4 m = glm::translate(glm::mat4(1.0F), glm::vec3(10.0F, 1.0F, 0.0F));
    m = glm::rotate(m, glm::radians(90.0F), glm::vec3(0.0F, 1.0F, 0.0F));
    m = glm::scale(m, glm::vec3(2.0F, 1.0F, 4.0F));
    const Aabb b = s::transform_bounds(unit, m);
    expect_vec3(b.min, {8.0F, 0.5F, -1.0F});
    expect_vec3(b.max, {12.0F, 1.5F, 1.0F});

    // 45 degrees about Y: a unit cube's footprint grows to the diagonal.
    const Aabb d = s::transform_bounds(
        unit, glm::rotate(glm::mat4(1.0F), glm::radians(45.0F), glm::vec3(0.0F, 1.0F, 0.0F)));
    expect_vec3(d.max, {0.70711F, 0.5F, 0.70711F});

    EXPECT_TRUE(s::bounds_empty(s::transform_bounds(s::empty_bounds(), m)));
}

TEST(Bounds, DrawablesContribute) {
    entt::registry r;
    const entt::entity cube = r.create();
    r.emplace<s::Transform>(cube);
    EXPECT_FALSE(s::local_draw_bounds(r, cube)) << "nothing drawn yet";

    r.emplace<s::Primitive>(cube).shape = s::PrimitiveShape::capsule;
    auto b = s::local_draw_bounds(r, cube);
    ASSERT_TRUE(b);
    expect_vec3(b->min, {-0.5F, -1.0F, -0.5F});

    // An instanced primitive: the union of every placement.
    const entt::entity rocks = r.create();
    r.emplace<s::Transform>(rocks).world =
        glm::translate(glm::mat4(1.0F), glm::vec3(0.0F, 0.0F, 100.0F));
    auto& im = r.emplace<s::InstancedMesh>(rocks);
    im.primitive.shape = s::PrimitiveShape::cube;
    im.instances.push_back({.position = {-5.0F, 0.0F, 0.0F}});
    im.instances.push_back({.position = {5.0F, 1.0F, 0.0F}, .scale = glm::vec3(3.0F)});
    b = s::local_draw_bounds(r, rocks);
    ASSERT_TRUE(b);
    expect_vec3(b->min, {-5.5F, -0.5F, -1.5F});
    expect_vec3(b->max, {6.5F, 2.5F, 1.5F});

    // The scene: both, through their world matrices.
    const auto scene = s::scene_draw_bounds(r);
    ASSERT_TRUE(scene);
    expect_vec3(scene->min, {-5.5F, -1.0F, -0.5F});
    expect_vec3(scene->max, {6.5F, 2.5F, 101.5F});
}

} // namespace
