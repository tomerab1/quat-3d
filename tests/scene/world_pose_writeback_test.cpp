// Systems that simulate in world space write back through the parent's space
// and keep the entity's rotation and scale (S1 audit B14): character
// controllers, kinematic nav agents, set_world_transform itself.

#include <entt/entity/registry.hpp>
#include <glm/glm.hpp>
#include <glm/gtc/matrix_transform.hpp>
#include <gtest/gtest.h>

#include "engine/physics/physics_world.hpp"
#include "engine/scene/components.hpp"
#include "engine/scene/navigation.hpp"
#include "engine/scene/scene.hpp"
#include "nav/nav_fixtures.hpp"
#include "support/expect_ok.hpp"

namespace {

namespace s = engine::scene;
using engine::physics::PhysicsWorld;

void expect_mat_near(const glm::mat4& a, const glm::mat4& b, float eps = 1e-4F) {
    for (int c = 0; c < 4; ++c) {
        for (int r = 0; r < 4; ++r) {
            EXPECT_NEAR(a[c][r], b[c][r], eps) << "column " << c << " row " << r;
        }
    }
}

// Parent at `at`, rotated 90 degrees about +Y; children sit at an offset in
// the parent's frame with uniform scale 2.
entt::entity make_rotated_parent(s::Scene& scene, const glm::vec3& at) {
    const entt::entity p = scene.create_entity("parent");
    scene.registry().get<s::Transform>(p).local =
        glm::translate(glm::mat4(1.0F), at) *
        glm::rotate(glm::mat4(1.0F), glm::radians(90.0F), glm::vec3(0.0F, 1.0F, 0.0F));
    return p;
}

entt::entity make_scaled_child(s::Scene& scene, entt::entity parent, const glm::vec3& offset) {
    const entt::entity c = scene.create_entity("child");
    scene.registry().get<s::Transform>(c).local =
        glm::translate(glm::mat4(1.0F), offset) * glm::scale(glm::mat4(1.0F), glm::vec3(2.0F));
    EXPECT_TRUE(scene.set_parent(c, parent));
    return c;
}

// The rotation/scale block of a world matrix (upper 3x3).
glm::mat3 basis(const glm::mat4& m) { return glm::mat3(m); }

TEST(WorldPoseWriteback, SetWorldTransformKeepsTheParentSpace) {
    s::Scene scene;
    const entt::entity p = make_rotated_parent(scene, {5.0F, 0.0F, 0.0F});
    const entt::entity c = make_scaled_child(scene, p, {1.0F, 0.0F, 0.0F});
    s::transform_system(scene.registry());

    const glm::mat4 target = glm::translate(glm::mat4(1.0F), glm::vec3(-3.0F, 4.0F, 7.0F)) *
                             glm::scale(glm::mat4(1.0F), glm::vec3(0.5F));
    s::set_world_transform(scene.registry(), c, target);
    expect_mat_near(scene.registry().get<s::Transform>(c).world, target);

    // Re-propagating reproduces the pose instead of re-applying the parent.
    s::transform_system(scene.registry());
    expect_mat_near(scene.registry().get<s::Transform>(c).world, target);

    // A root's local is its world.
    s::set_world_transform(scene.registry(), p, target);
    expect_mat_near(scene.registry().get<s::Transform>(p).local, target);
}

// A fly camera driving an authored, parented scene camera (non-editor
// builds) places it in world space: the view is the requested one, not the
// parent's transform applied on top of it.
TEST(WorldPoseWriteback, ParentedCameraViewsFromTheRequestedPose) {
    s::Scene scene;
    const entt::entity p = make_rotated_parent(scene, {5.0F, 2.0F, -1.0F});
    const entt::entity cam = scene.create_entity("camera");
    scene.registry().emplace<s::Camera>(cam);
    ASSERT_TRUE(scene.set_parent(cam, p));
    scene.tick();

    const glm::vec3 eye(-4.0F, 3.0F, 6.0F);
    const glm::mat4 view = glm::lookAt(eye, glm::vec3(0.0F), glm::vec3(0.0F, 1.0F, 0.0F));
    s::set_world_transform(scene.registry(), cam, glm::inverse(view));
    scene.tick(); // propagation must reproduce the pose
    const s::CameraMatrices m = s::camera_system(scene.registry(), 16.0F / 9.0F);
    EXPECT_NEAR(glm::distance(m.position, eye), 0.0F, 1e-4F);
    expect_mat_near(m.view, view);
}

TEST(WorldPoseWriteback, CharacterKeepsRotationScaleAndParentSpace) {
    auto created = PhysicsWorld::create();
    ASSERT_OK(created);
    PhysicsWorld& world = *created;
    PhysicsWorld::BodyParams floor;
    floor.shape = world.create_box(glm::vec3(50.0F, 1.0F, 50.0F));
    floor.position = glm::vec3(0.0F, -1.0F, 0.0F);
    floor.motion = engine::physics::Motion::static_body;
    floor.layer = engine::physics::Layer::static_body;
    ASSERT_NE(world.add_body(floor), PhysicsWorld::invalid_body);

    s::Scene scene;
    entt::registry& r = scene.registry();
    const entt::entity p = make_rotated_parent(scene, {5.0F, 0.0F, 0.0F});
    const entt::entity c = make_scaled_child(scene, p, {0.0F, 3.0F, 0.0F});
    r.emplace<s::CharacterController>(c).move = glm::vec3(1.0F, 0.0F, 0.0F);
    s::transform_system(r);
    const glm::mat3 basis_before = basis(r.get<s::Transform>(c).world);

    for (int i = 0; i < 120; ++i) {
        s::character_system(r, world, 1.0F / 60.0F);
        s::transform_system(r); // must not move the character back into the parent's frame
    }

    const auto& t = r.get<s::Transform>(c);
    const glm::vec3 pos = world.character_position(r.get<s::CharacterController>(c).character);
    EXPECT_NEAR(t.world[3].x, pos.x, 1e-3F);
    EXPECT_NEAR(t.world[3].y, pos.y, 1e-3F);
    EXPECT_NEAR(t.world[3].z, pos.z, 1e-3F);
    EXPECT_GT(pos.x, 6.0F) << "walked +x in world space";
    EXPECT_LT(pos.y, 2.5F) << "fell onto the floor";
    const glm::mat3 basis_after = basis(t.world);
    for (int col = 0; col < 3; ++col) {
        for (int row = 0; row < 3; ++row) {
            EXPECT_NEAR(basis_after[col][row], basis_before[col][row], 1e-4F)
                << "rotation/scale discarded";
        }
    }
}

TEST(WorldPoseWriteback, KinematicNavAgentKeepsRotationScaleAndParentSpace) {
    quat::test::NavWorld w;
    quat::test::add_box(w.registry, glm::vec3(0.0F, -0.5F, 0.0F), glm::vec3(20.0F, 0.5F, 20.0F));
    w.bake();

    // A parented, rotated + scaled agent (no CharacterController: kinematic).
    const entt::entity p = w.registry.create();
    auto& pt = w.registry.emplace<s::Transform>(p);
    pt.local = glm::translate(glm::mat4(1.0F), glm::vec3(-5.0F, 0.0F, 0.0F)) *
               glm::rotate(glm::mat4(1.0F), glm::radians(90.0F), glm::vec3(0.0F, 1.0F, 0.0F));
    const entt::entity e = w.registry.create();
    auto& et = w.registry.emplace<s::Transform>(e);
    et.local = glm::scale(glm::mat4(1.0F), glm::vec3(2.0F));
    w.registry.emplace<s::Parent>(e, p);
    w.registry.emplace<s::Children>(p).entities.push_back(e);
    w.registry.emplace<s::NavAgent>(e).speed = 4.0F;
    s::transform_system(w.registry);
    const glm::mat3 basis_before = basis(w.registry.get<s::Transform>(e).world);

    const glm::vec3 goal(5.0F, 0.0F, 5.0F);
    s::nav_agent_set_target(w.nav(e), goal);
    for (int i = 0; i < 60 * 20 && w.nav(e).active; ++i) {
        s::nav_agent_system(w.registry, w.navmesh, quat::test::kNavDt);
        s::transform_system(w.registry);
    }
    EXPECT_TRUE(w.nav(e).arrived);
    const auto& t = w.registry.get<s::Transform>(e);
    EXPECT_LT(quat::test::xz_distance(glm::vec3(t.world[3]), goal), 1.0F)
        << "the parent's transform was re-applied to a world-space position";
    const glm::mat3 basis_after = basis(t.world);
    for (int col = 0; col < 3; ++col) {
        for (int row = 0; row < 3; ++row) {
            EXPECT_NEAR(basis_after[col][row], basis_before[col][row], 1e-4F);
        }
    }
}

} // namespace
