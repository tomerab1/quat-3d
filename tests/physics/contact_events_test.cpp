// PhysicsWorld contact events (A10.1): one event per body pair, impact
// impulse estimates, sleeping does not end a contact, removal does, sensors
// report trigger enter / exit sensor-first, and a large pile collected on
// Jolt's worker threads stays consistent. publish_physics_events feeds the
// engine event ring with entity ids.

#include <algorithm>
#include <cstdint>
#include <map>
#include <optional>
#include <utility>
#include <vector>

#include <glm/glm.hpp>
#include <gtest/gtest.h>

#include "engine/core/event_ring.hpp"
#include "engine/physics/physics_world.hpp"
#include "engine/scene/scene.hpp"
#include "support/expect_ok.hpp"

namespace {

using engine::physics::Layer;
using engine::physics::Motion;
using engine::physics::PhysicsWorld;
using Kind = PhysicsWorld::ContactEvent::Kind;

constexpr float         k_dt = 1.0F / 60.0F;
constexpr std::uint32_t k_floor_owner = 100;

class ContactEvents : public ::testing::Test {
protected:
    void SetUp() override {
        auto w = PhysicsWorld::create();
        ASSERT_OK(w);
        world.emplace(std::move(*w));
    }

    std::uint32_t add_floor() {
        PhysicsWorld::BodyParams p;
        p.shape = world->create_box(glm::vec3(50.0F, 1.0F, 50.0F));
        p.position = glm::vec3(0.0F, -1.0F, 0.0F);
        p.motion = Motion::static_body;
        p.layer = Layer::static_body;
        p.owner = k_floor_owner;
        return world->add_body(p);
    }

    std::uint32_t add_box(const glm::vec3& pos, std::uint32_t owner, float half = 0.5F) {
        PhysicsWorld::BodyParams p;
        p.shape = world->create_box(glm::vec3(half));
        p.position = pos;
        p.mass = 1.0F;
        p.owner = owner;
        return world->add_body(p);
    }

    // Steps `frames` frames, collecting every event.
    void step(int frames) {
        for (int i = 0; i < frames; ++i) {
            world->update(k_dt);
            for (const auto& ev : world->contact_events()) events.push_back(ev);
            dropped += world->dropped_contact_events();
        }
    }

    [[nodiscard]] int count(Kind kind, std::uint32_t a, std::uint32_t b) const {
        int n = 0;
        for (const auto& ev : events) {
            const bool pair = (ev.body_a == a && ev.body_b == b) || (ev.body_a == b && ev.body_b == a);
            n += ev.kind == kind && pair ? 1 : 0;
        }
        return n;
    }

    std::optional<PhysicsWorld>               world;
    std::vector<PhysicsWorld::ContactEvent> events;
    std::uint32_t                             dropped = 0;
};

TEST_F(ContactEvents, ALandingBoxReportsOneContactWithItsImpact) {
    const std::uint32_t floor = add_floor();
    const std::uint32_t box = add_box(glm::vec3(0.0F, 2.0F, 0.0F), 7);
    step(90);
    ASSERT_EQ(count(Kind::contact_added, box, floor), 1);
    EXPECT_EQ(count(Kind::contact_removed, box, floor), 0);
    const PhysicsWorld::ContactEvent* hit = nullptr;
    for (const auto& ev : events) {
        if (ev.kind == Kind::contact_added) hit = &ev;
    }
    ASSERT_NE(hit, nullptr);
    // Fell 1.5 m: ~5.4 m/s into the floor, 1 kg, no restitution.
    EXPECT_NEAR(hit->impulse, 5.4F, 1.5F);
    EXPECT_NEAR(hit->point.y, 0.0F, 0.1F);
    // The normal points from body a towards body b.
    const float towards_b = hit->body_a == floor ? hit->normal.y : -hit->normal.y;
    EXPECT_GT(towards_b, 0.9F);
    EXPECT_EQ(hit->body_a == box ? hit->owner_a : hit->owner_b, 7U);
    EXPECT_EQ(hit->body_a == floor ? hit->owner_a : hit->owner_b, k_floor_owner);
    EXPECT_GT(hit->step, 0U);
    EXPECT_EQ(dropped, 0U);
}

TEST_F(ContactEvents, SleepingDoesNotEndAContactButParting) {
    const std::uint32_t floor = add_floor();
    const std::uint32_t box = add_box(glm::vec3(0.0F, 0.6F, 0.0F), 7);
    step(300); // 5 s: lands and falls asleep (Jolt then drops its contacts)
    ASSERT_EQ(world->active_body_count(), 0U) << "the box never slept";
    EXPECT_EQ(count(Kind::contact_added, box, floor), 1);
    EXPECT_EQ(count(Kind::contact_removed, box, floor), 0) << "sleep ended the contact";

    // Woken and lifted away: the contact ends.
    world->set_body_transform(box, glm::vec3(0.0F, 5.0F, 0.0F), glm::quat(1.0F, 0.0F, 0.0F, 0.0F));
    step(2);
    EXPECT_EQ(count(Kind::contact_removed, box, floor), 1);
    // ...and it lands again: a new contact.
    step(120);
    EXPECT_EQ(count(Kind::contact_added, box, floor), 2);
}

TEST_F(ContactEvents, RemovingABodyEndsItsContacts) {
    const std::uint32_t floor = add_floor();
    const std::uint32_t sleeper = add_box(glm::vec3(-3.0F, 0.6F, 0.0F), 7);
    step(300);
    ASSERT_EQ(world->active_body_count(), 0U);
    const std::uint32_t awake = add_box(glm::vec3(3.0F, 0.6F, 0.0F), 8);
    step(20); // landed, still awake
    world->remove_body(sleeper);
    world->remove_body(awake);
    step(1);
    EXPECT_EQ(count(Kind::contact_removed, sleeper, floor), 1);
    EXPECT_EQ(count(Kind::contact_removed, awake, floor), 1);
    for (const auto& ev : events) {
        if (ev.kind == Kind::contact_removed) { // owners survive the body
            EXPECT_EQ(ev.body_a == floor ? ev.owner_b : ev.owner_a, ev.body_a == sleeper ||
                                                                            ev.body_b == sleeper
                                                                        ? 7U
                                                                        : 8U);
        }
    }
}

TEST_F(ContactEvents, ManyTouchingTrianglesAreOneContact) {
    // A flat height field: a 2 m box lands on several triangles at once.
    std::vector<float> flat(16 * 16, 0.0F);
    PhysicsWorld::BodyParams ground;
    ground.shape = world->create_height_field(flat, 16, glm::vec3(-8.0F, 0.0F, -8.0F),
                                              glm::vec3(1.0F));
    ground.motion = Motion::static_body;
    ground.layer = Layer::static_body;
    const std::uint32_t field = world->add_body(ground);
    ASSERT_NE(field, PhysicsWorld::invalid_body);
    const std::uint32_t box = add_box(glm::vec3(0.3F, 2.0F, 0.3F), 7, 1.0F);
    step(120);
    EXPECT_EQ(count(Kind::contact_added, box, field), 1);
    EXPECT_EQ(count(Kind::contact_removed, box, field), 0);
}

TEST_F(ContactEvents, SensorsReportEnterAndExitSensorFirst) {
    PhysicsWorld::BodyParams trigger;
    trigger.shape = world->create_box(glm::vec3(1.0F));
    trigger.position = glm::vec3(0.0F, 3.0F, 0.0F);
    trigger.motion = Motion::static_body;
    trigger.layer = Layer::sensor;
    trigger.sensor = true;
    const std::uint32_t sensor = world->add_body(trigger);
    const std::uint32_t ball = add_box(glm::vec3(0.0F, 6.0F, 0.0F), 9, 0.25F);
    step(150);
    int enters = 0;
    int exits = 0;
    for (const auto& ev : events) {
        if (ev.kind == Kind::trigger_enter || ev.kind == Kind::trigger_exit) {
            EXPECT_EQ(ev.body_a, sensor);
            EXPECT_EQ(ev.body_b, ball);
            (ev.kind == Kind::trigger_enter ? enters : exits) += 1;
        }
    }
    EXPECT_EQ(enters, 1);
    EXPECT_EQ(exits, 1);
}

TEST_F(ContactEvents, APileCollectedOnWorkerThreadsStaysConsistent) {
    add_floor();
    for (int i = 0; i < 300; ++i) {
        const float x = static_cast<float>(i % 10) * 0.9F - 4.5F;
        const float z = static_cast<float>((i / 10) % 10) * 0.9F - 4.5F;
        const float y = 1.0F + static_cast<float>(i / 100) * 1.2F;
        add_box(glm::vec3(x, y, z), static_cast<std::uint32_t>(i));
    }
    step(240);
    EXPECT_EQ(dropped, 0U);
    std::map<std::pair<std::uint32_t, std::uint32_t>, int> open;
    for (const auto& ev : events) {
        const auto key = std::minmax(ev.body_a, ev.body_b);
        open[key] += ev.kind == Kind::contact_added ? 1 : ev.kind == Kind::contact_removed ? -1 : 0;
        ASSERT_GE(open[key], 0) << "removed before added";
        ASSERT_LE(open[key], 1) << "added twice";
    }
    EXPECT_GT(open.size(), 300U);
}

TEST_F(ContactEvents, PublishingFeedsTheEventRingWithEntities) {
    add_floor();
    add_box(glm::vec3(0.0F, 2.0F, 0.0F), 42);
    engine::core::EventRing ring(64);
    for (int i = 0; i < 90; ++i) {
        world->update(k_dt);
        engine::scene::publish_physics_events(*world, ring);
    }
    const auto q = ring.query(0, 64);
    ASSERT_EQ(q.events.size(), 1U);
    const engine::core::Event& e = q.events[0];
    EXPECT_EQ(e.kind, engine::core::EventKind::contact_added);
    EXPECT_TRUE((e.entity_a == 42 && e.entity_b == k_floor_owner) ||
                (e.entity_b == 42 && e.entity_a == k_floor_owner));
    EXPECT_GT(e.impulse, 1.0F);
    EXPECT_GT(e.time_s, 0.0);
}

} // namespace
