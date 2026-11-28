// Characters collide with each other and with dynamic bodies (issue #48):
// every CharacterVirtual is registered with a CharacterVsCharacterCollision,
// and carries an inner body that dynamic bodies and sensors see.

#include <algorithm>
#include <cstdint>
#include <optional>
#include <utility>

#include <entt/entity/registry.hpp>
#include <glm/glm.hpp>
#include <glm/gtc/matrix_transform.hpp>
#include <gtest/gtest.h>

#include "engine/physics/physics_world.hpp"
#include "engine/scene/components.hpp"
#include "engine/scene/scene.hpp"
#include "support/expect_ok.hpp"

namespace {

namespace s = engine::scene;
using engine::physics::PhysicsWorld;

constexpr float k_dt = 1.0F / 60.0F;

class CharacterCollision : public ::testing::Test {
protected:
    void SetUp() override {
        auto w = PhysicsWorld::create();
        ASSERT_OK(w);
        world.emplace(std::move(*w));
        // Static floor entity (top at y = 0).
        const entt::entity floor = place(glm::vec3(0.0F, -1.0F, 0.0F));
        registry.emplace<s::Collider>(floor, s::ColliderShape::box,
                                      glm::vec3(50.0F, 1.0F, 50.0F), glm::vec3(0.0F));
        registry.emplace<s::RigidBody>(floor, s::BodyMotion::static_body, 1.0F,
                                       PhysicsWorld::invalid_body);
    }

    entt::entity place(const glm::vec3& pos) {
        const entt::entity e = registry.create();
        auto& t = registry.emplace<s::Transform>(e);
        t.local = glm::translate(glm::mat4(1.0F), pos);
        t.world = t.local;
        return e;
    }

    entt::entity add_character(const glm::vec3& pos) {
        const entt::entity e = place(pos);
        registry.emplace<s::CharacterController>(e); // radius 0.3, half-height 0.6
        return e;
    }

    // One frame as the application runs it: characters, then bodies.
    void step(int frames) {
        for (int i = 0; i < frames; ++i) {
            s::character_system(registry, *world, k_dt);
            s::physics_system(registry, *world, k_dt);
        }
    }

    [[nodiscard]] glm::vec3 position(entt::entity e) const {
        return glm::vec3(registry.get<s::Transform>(e).world[3]);
    }

    std::optional<PhysicsWorld> world;
    entt::registry              registry;
};

TEST_F(CharacterCollision, TwoCharactersWalkingIntoEachOtherStaySeparated) {
    const entt::entity a = add_character(glm::vec3(-2.0F, 1.0F, 0.0F));
    const entt::entity b = add_character(glm::vec3(2.0F, 1.0F, 0.0F));
    step(60); // land
    registry.get<s::CharacterController>(a).move = glm::vec3(1.0F, 0.0F, 0.0F);
    registry.get<s::CharacterController>(b).move = glm::vec3(-1.0F, 0.0F, 0.0F);
    const float radius = registry.get<s::CharacterController>(a).radius;
    float closest = 1e9F;
    for (int i = 0; i < 180; ++i) { // 3 s: without collisions they pass through
        step(1);
        const glm::vec3 d = position(b) - position(a);
        closest = std::min(closest, glm::length(glm::vec2(d.x, d.z)));
    }
    EXPECT_GE(closest, 2.0F * radius - 0.05F) << "the capsules interpenetrated";
    EXPECT_LT(position(a).x, position(b).x) << "the characters passed through each other";
}

TEST_F(CharacterCollision, ADynamicBoxDroppedOnACharacterRestsOnIt) {
    const entt::entity c = add_character(glm::vec3(0.0F, 1.0F, 0.0F));
    step(60); // land: the capsule centre sits at ~0.9, its top at ~1.8
    const float top = position(c).y + 0.9F;
    // An idle character whose controller is not stepped this frame (a
    // network player between inputs) still blocks bodies: only the inner
    // body is in the simulation then.
    registry.emplace<s::NetDriven>(c);

    const entt::entity box = place(glm::vec3(0.0F, top + 1.5F, 0.0F));
    registry.emplace<s::Collider>(box, s::ColliderShape::box, glm::vec3(0.25F), glm::vec3(0.0F));
    registry.emplace<s::RigidBody>(box, s::BodyMotion::dynamic, 1.0F, PhysicsWorld::invalid_body);
    step(66); // 1.1 s: lands after ~0.55 s; without a collision it reaches the floor by ~0.7 s
    EXPECT_GT(position(box).y, top) << "the box fell through the character";
    EXPECT_NEAR(position(box).y, top + 0.25F, 0.15F) << "the box is not resting on the capsule";
}

TEST_F(CharacterCollision, SensorsDetectCharacters) {
    PhysicsWorld::BodyParams trigger;
    trigger.shape = world->create_box(glm::vec3(1.0F));
    trigger.position = glm::vec3(3.0F, 1.0F, 0.0F);
    trigger.motion = engine::physics::Motion::static_body;
    trigger.layer = engine::physics::Layer::sensor;
    trigger.sensor = true;
    const std::uint32_t sensor = world->add_body(trigger);
    ASSERT_NE(sensor, PhysicsWorld::invalid_body);

    const entt::entity c = add_character(glm::vec3(0.0F, 1.0F, 0.0F));
    step(60);
    registry.get<s::CharacterController>(c).move = glm::vec3(1.0F, 0.0F, 0.0F);
    bool entered = false;
    for (int i = 0; i < 90 && !entered; ++i) {
        step(1);
        for (const PhysicsWorld::TriggerEvent& ev : world->trigger_events()) {
            entered = entered || (ev.entered && ev.trigger == sensor &&
                                  world->body_owner(ev.other) ==
                                      static_cast<std::uint32_t>(entt::to_integral(c)));
        }
    }
    EXPECT_TRUE(entered) << "the sensor never saw the character";
}

TEST_F(CharacterCollision, RemovingACharacterUnregistersIt) {
    const entt::entity a = add_character(glm::vec3(-2.0F, 1.0F, 0.0F));
    const entt::entity b = add_character(glm::vec3(2.0F, 1.0F, 0.0F));
    step(30);
    EXPECT_EQ(world->character_count(), 2U);
    EXPECT_EQ(world->body_count(), 1U) << "inner bodies are not counted as bodies";
    registry.destroy(b);
    step(1);
    EXPECT_EQ(world->character_count(), 1U);
    // `a` now walks freely through where `b` stood.
    registry.get<s::CharacterController>(a).move = glm::vec3(1.0F, 0.0F, 0.0F);
    step(90);
    EXPECT_GT(position(a).x, 2.5F);
}

} // namespace
