// Physics control over remote (A10.1): events.poll.

#include <cstdint>
#include <optional>
#include <string>
#include <utility>

#include <glm/glm.hpp>
#include <gtest/gtest.h>

#include "engine/core/event_ring.hpp"
#include "engine/physics/physics_world.hpp"
#include "remote/world_fixture.hpp"
#include "support/expect_ok.hpp"

namespace {

namespace core = engine::core;
namespace rpc_code = engine::remote::rpc_code;
using engine::physics::PhysicsWorld;
using quat::test::Json;

class PhysicsCommands : public quat::test::WorldCommands {
protected:
    core::EventRing             ring{8};
    std::optional<PhysicsWorld> physics;

    void SetUp() override {
        services.event_ring = &ring;
        services.physics_control = [this]() -> PhysicsWorld* {
            return physics ? &*physics : nullptr;
        };
        register_commands();
    }

    void push(core::EventKind kind, std::uint32_t a, std::uint32_t b) {
        core::Event e;
        e.kind = kind;
        e.entity_a = a;
        e.entity_b = b;
        e.point = {1.0F, 2.0F, 3.0F};
        e.normal = {0.0F, 1.0F, 0.0F};
        e.impulse = 4.5F;
        ring.push(e);
    }
};

TEST_F(PhysicsCommands, EventsPollTailsTheRing) {
    const auto crate = static_cast<std::uint32_t>(entity(ok("scene.create", {{"name", "Crate"}})["id"]));
    push(core::EventKind::contact_added, crate, core::Event::no_entity);
    push(core::EventKind::trigger_enter, 77, crate);

    Json r = ok("events.poll");
    ASSERT_EQ(r["events"].size(), 2U);
    const Json& first = r["events"][0];
    EXPECT_EQ(first["kind"], "contact_added");
    EXPECT_EQ(first["seq"], 1);
    EXPECT_EQ(first["entity_a"], crate);
    EXPECT_TRUE(first["entity_b"].is_null()) << "unowned bodies (terrain) are null";
    EXPECT_EQ(first["name_a"], "Crate");
    EXPECT_EQ(first["normal"], Json::array({0.0, 1.0, 0.0}));
    EXPECT_FLOAT_EQ(first["impulse"].get<float>(), 4.5F);
    EXPECT_FALSE(r["events"][1].contains("impulse")) << "only contacts carry an impulse";
    EXPECT_EQ(r["cursor"], 2);
    EXPECT_EQ(r["dropped"], 0);
    EXPECT_FALSE(r["more"].get<bool>());

    push(core::EventKind::contact_removed, crate, 5);
    r = ok("events.poll", {{"since", 2}});
    ASSERT_EQ(r["events"].size(), 1U);
    EXPECT_EQ(r["events"][0]["kind"], "contact_removed");
    EXPECT_FALSE(r["events"][0].contains("point"));
}

TEST_F(PhysicsCommands, EventsPollFiltersAndPages) {
    for (std::uint32_t i = 0; i < 4; ++i) push(core::EventKind::contact_added, i, 50);
    push(core::EventKind::trigger_enter, 9, 50);
    Json r = ok("events.poll", {{"kinds", {"trigger_enter"}}});
    ASSERT_EQ(r["events"].size(), 1U);
    EXPECT_EQ(r["events"][0]["entity_a"], 9);
    EXPECT_EQ(r["cursor"], 5);

    r = ok("events.poll", {{"max", 2}});
    ASSERT_EQ(r["events"].size(), 2U);
    EXPECT_TRUE(r["more"].get<bool>());
    EXPECT_EQ(r["cursor"], 2);
    r = ok("events.poll", {{"since", r["cursor"]}, {"max", 2}});
    EXPECT_EQ(r["events"][0]["seq"], 3);
}

TEST_F(PhysicsCommands, EventsPollReportsDroppedEvents) {
    for (std::uint32_t i = 0; i < 12; ++i) push(core::EventKind::contact_added, i, i);
    ring.drop(3);
    const Json r = ok("events.poll");
    EXPECT_EQ(r["events"].size(), 8U);
    EXPECT_EQ(r["dropped"], 7) << "4 overwritten + 3 never recorded";
    EXPECT_EQ(r["cursor"], 15);
    EXPECT_EQ(r["oldest_seq"], 5);
}

TEST_F(PhysicsCommands, EventsPollValidatesItsParameters) {
    EXPECT_NE(err("events.poll", {{"kinds", {"explosion"}}}, rpc_code::invalid_params)
                  .find("contact_added"),
              std::string::npos);
    err("events.poll", {{"max", 0}}, rpc_code::invalid_params);
    err("events.poll", {{"since", -1}}, rpc_code::invalid_params);
    err("events.poll", {{"sinse", 3}}, rpc_code::invalid_params);
    err("events.poll", {{"entity", "Nobody"}}, rpc_code::not_found);
}

TEST_F(PhysicsCommands, EventsPollFiltersByEntity) {
    const Json a = ok("scene.create", {{"name", "A"}})["id"];
    push(core::EventKind::contact_added, 3, 4);
    push(core::EventKind::contact_added, 4, a.get<std::uint32_t>());
    const Json r = ok("events.poll", {{"entity", "A"}});
    ASSERT_EQ(r["events"].size(), 1U);
    EXPECT_EQ(r["events"][0]["entity_b"], a);
}

} // namespace
