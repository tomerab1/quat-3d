// PhysicsWorld multi-hit queries (A10.1): every hit along a ray, shape
// overlaps and shape casts, with layer masks, characters reported through
// their inner bodies, and hit limits.

#include <cstdint>
#include <optional>
#include <utility>

#include <glm/glm.hpp>
#include <gtest/gtest.h>

#include "engine/physics/physics_world.hpp"
#include "support/expect_ok.hpp"

namespace {

using engine::physics::Layer;
using engine::physics::layer_bit;
using engine::physics::Motion;
using engine::physics::PhysicsWorld;
using Shape = PhysicsWorld::QueryShape;

const glm::quat k_identity{1.0F, 0.0F, 0.0F, 0.0F};

class Queries : public ::testing::Test {
protected:
    void SetUp() override {
        auto w = PhysicsWorld::create();
        ASSERT_OK(w);
        world.emplace(std::move(*w));
        PhysicsWorld::BodyParams floor;
        floor.shape = world->create_box(glm::vec3(50.0F, 1.0F, 50.0F));
        floor.position = glm::vec3(0.0F, -1.0F, 0.0F);
        floor.motion = Motion::static_body;
        floor.layer = Layer::static_body;
        floor_body = world->add_body(floor);
    }

    std::uint32_t box(const glm::vec3& pos, std::uint32_t owner, Motion motion = Motion::static_body,
                      Layer layer = Layer::static_body, bool sensor = false) {
        PhysicsWorld::BodyParams p;
        p.shape = world->create_box(glm::vec3(0.5F));
        p.position = pos;
        p.motion = motion;
        p.layer = layer;
        p.sensor = sensor;
        p.owner = owner;
        return world->add_body(p);
    }

    std::optional<PhysicsWorld> world;
    std::uint32_t               floor_body = PhysicsWorld::invalid_body;
};

TEST_F(Queries, RaycastAllReturnsEveryBodyNearestFirst) {
    const std::uint32_t a = box(glm::vec3(0.0F, 2.0F, 0.0F), 1);
    const std::uint32_t b = box(glm::vec3(0.0F, 5.0F, 0.0F), 2);
    const auto hits = world->cast_ray_all(glm::vec3(0.0F, 10.0F, 0.0F), glm::vec3(0.0F, -1.0F, 0.0F),
                                          20.0F);
    ASSERT_EQ(hits.size(), 3U);
    EXPECT_EQ(hits[0].body, b);
    EXPECT_EQ(hits[1].body, a);
    EXPECT_EQ(hits[2].body, floor_body);
    EXPECT_NEAR(hits[0].distance, 4.5F, 1e-3F);
    EXPECT_NEAR(hits[0].fraction, 4.5F / 20.0F, 1e-4F);
    EXPECT_NEAR(hits[0].point.y, 5.5F, 1e-3F);
    EXPECT_NEAR(hits[0].normal.y, 1.0F, 1e-3F);
    EXPECT_EQ(hits[0].owner, 2U);
    EXPECT_EQ(hits[2].owner, PhysicsWorld::no_owner);
    EXPECT_EQ(world->cast_ray_all(glm::vec3(0.0F, 10.0F, 0.0F), glm::vec3(0.0F, -1.0F, 0.0F),
                                  20.0F, engine::physics::solid_layers, 1)
                  .size(),
              1U);
}

TEST_F(Queries, LayerMasksFilterHits) {
    box(glm::vec3(0.0F, 2.0F, 0.0F), 1, Motion::dynamic, Layer::dynamic);
    const std::uint32_t trigger = box(glm::vec3(0.0F, 5.0F, 0.0F), 2, Motion::static_body,
                                      Layer::sensor, true);
    const glm::vec3 from(0.0F, 10.0F, 0.0F);
    const glm::vec3 down(0.0F, -1.0F, 0.0F);
    // Default: solid layers only, the trigger volume is skipped.
    EXPECT_EQ(world->cast_ray_all(from, down, 20.0F).size(), 2U);
    const auto statics = world->cast_ray_all(from, down, 20.0F, layer_bit(Layer::static_body));
    ASSERT_EQ(statics.size(), 1U);
    EXPECT_EQ(statics[0].body, floor_body);
    const auto sensors = world->cast_ray_all(from, down, 20.0F, layer_bit(Layer::sensor));
    ASSERT_EQ(sensors.size(), 1U);
    EXPECT_EQ(sensors[0].body, trigger);
    EXPECT_TRUE(sensors[0].sensor);
    EXPECT_EQ(sensors[0].layer, Layer::sensor);
}

TEST_F(Queries, CharactersAreHitThroughTheirInnerBodies) {
    const std::uint32_t c = world->create_character(0.6F, 0.3F, glm::vec3(4.0F, 1.0F, 0.0F), 77);
    const auto hits = world->cast_ray_all(glm::vec3(0.0F, 1.0F, 0.0F), glm::vec3(1.0F, 0.0F, 0.0F),
                                          10.0F);
    ASSERT_EQ(hits.size(), 1U);
    EXPECT_EQ(hits[0].character, c);
    EXPECT_EQ(hits[0].owner, 77U);
    EXPECT_EQ(hits[0].layer, Layer::character);
    EXPECT_NEAR(hits[0].point.x, 3.7F, 0.02F);
}

TEST_F(Queries, OverlapFindsTouchingBodiesDeepestFirst) {
    const std::uint32_t near_box = box(glm::vec3(0.0F, 0.5F, 0.0F), 1);
    const std::uint32_t far_box = box(glm::vec3(1.3F, 0.5F, 0.0F), 2);
    box(glm::vec3(10.0F, 0.5F, 0.0F), 3); // out of reach
    PhysicsWorld::ShapeDesc sphere;
    sphere.radius = 1.0F;
    const auto hits = world->overlap(sphere, glm::vec3(0.0F, 1.5F, 0.0F), k_identity,
                                     layer_bit(Layer::static_body));
    ASSERT_EQ(hits.size(), 2U); // both boxes, not the floor (the sphere ends at y = 0.5)
    EXPECT_EQ(hits[0].body, near_box);
    EXPECT_GT(hits[0].penetration, hits[1].penetration);
    bool far_seen = false;
    for (const auto& h : hits) far_seen = far_seen || h.body == far_box;
    EXPECT_TRUE(far_seen);

    PhysicsWorld::ShapeDesc small;
    small.type = Shape::box;
    small.half_extents = glm::vec3(0.1F);
    EXPECT_TRUE(world->overlap(small, glm::vec3(5.0F, 5.0F, 5.0F), k_identity).empty());
}

TEST_F(Queries, ShapeCastStopsAtTheFirstSurface) {
    const std::uint32_t wall = box(glm::vec3(5.0F, 1.0F, 0.0F), 9);
    PhysicsWorld::ShapeDesc capsule;
    capsule.type = Shape::capsule;
    capsule.radius = 0.25F;
    capsule.half_height = 0.5F;
    const auto hits = world->cast_shape(capsule, glm::vec3(0.0F, 1.5F, 0.0F),
                                        glm::vec3(10.0F, 1.5F, 0.0F), k_identity,
                                        layer_bit(Layer::static_body));
    ASSERT_FALSE(hits.empty());
    // The capsule (lowest point y = 0.75) clears the floor and meets the
    // wall's face at x = 4.5 after 4.25 m.
    EXPECT_EQ(hits[0].body, wall);
    EXPECT_NEAR(hits[0].distance, 4.25F, 0.02F);
    EXPECT_NEAR(hits[0].fraction, 0.425F, 0.002F);
    EXPECT_NEAR(hits[0].normal.x, -1.0F, 0.01F);
    EXPECT_NEAR(hits[0].point.x, 4.5F, 0.02F);
}

TEST_F(Queries, BadInputReturnsNothing) {
    PhysicsWorld::ShapeDesc bad;
    bad.radius = -1.0F;
    EXPECT_TRUE(world->overlap(bad, glm::vec3(0.0F), k_identity).empty());
    EXPECT_TRUE(world->cast_shape(PhysicsWorld::ShapeDesc{}, glm::vec3(1.0F), glm::vec3(1.0F),
                                  k_identity)
                    .empty());
    EXPECT_TRUE(world->cast_ray_all(glm::vec3(0.0F), glm::vec3(0.0F), 10.0F).empty());
}

} // namespace
