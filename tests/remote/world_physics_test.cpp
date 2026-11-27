// Physics control over remote (A10.1): events.poll, physics.get / set /
// apply, physics.get_gravity / set_gravity.

#include <cstdint>
#include <optional>
#include <string>
#include <utility>

#include <glm/glm.hpp>
#include <gtest/gtest.h>

#include "engine/core/event_ring.hpp"
#include "engine/physics/physics_world.hpp"
#include "engine/scene/physics_settings.hpp"
#include "engine/scene/scene.hpp"
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

// ---- physics.get / set / apply ---------------------------------------------

class BodyCommands : public PhysicsCommands {
protected:
    void SetUp() override {
        PhysicsCommands::SetUp();
        auto w = PhysicsWorld::create();
        ASSERT_OK(w);
        physics.emplace(std::move(*w));
    }

    // Creates an entity with the given components and builds its body.
    Json spawn(const std::string& name, const Json& components) {
        const Json id = ok("scene.create", {{"name", name}, {"components", components}})["id"];
        engine::scene::physics_system(reg(), *physics, 0.0F); // build, no step
        return id;
    }
    Json crate(const std::string& name = "Crate") {
        return spawn(name, {{"transform", {{"position", {0, 5, 0}}}},
                            {"collider", {{"shape", "box"}, {"half_extents", {0.5, 0.5, 0.5}}}},
                            {"rigidbody", {{"motion", "dynamic"}, {"mass", 2}}}});
    }
    void simulate(int frames) {
        for (int i = 0; i < frames; ++i) {
            engine::scene::physics_system(reg(), *physics, PhysicsWorld::fixed_timestep);
        }
    }
};

TEST_F(BodyCommands, GetReportsTheBodyState) {
    const Json id = crate();
    const Json s = ok("physics.get", {{"entity", "Crate"}});
    EXPECT_EQ(s["entity"], id);
    EXPECT_EQ(s["kind"], "body");
    EXPECT_EQ(s["motion_type"], "dynamic");
    EXPECT_EQ(s["layer"], "dynamic");
    EXPECT_FALSE(s["sleeping"].get<bool>());
    EXPECT_NEAR(s["mass"].get<float>(), 2.0F, 1e-4F);
    EXPECT_NEAR(s["position"][1].get<float>(), 5.0F, 1e-4F);
    EXPECT_NEAR(s["friction"].get<float>(), 0.2F, 1e-6F);
}

TEST_F(BodyCommands, SetChangesVelocityAndTuningAndPersistsAuthoringFields) {
    crate();
    const Json s = ok("physics.set", {{"entity", "Crate"},
                                      {"linear_velocity", {1, 2, 3}},
                                      {"angular_velocity", {0, 90, 0}},
                                      {"friction", 0.8},
                                      {"restitution", 0.5},
                                      {"gravity_factor", 0}});
    EXPECT_EQ(s["linear_velocity"], Json::array({1.0, 2.0, 3.0}));
    EXPECT_NEAR(s["angular_velocity"][1].get<float>(), 90.0F, 1e-3F);
    EXPECT_NEAR(s["restitution"].get<float>(), 0.5F, 1e-6F);
    // The authoring values land in the component (scene.get, scene files).
    const Json c = ok("scene.get", {{"entity", "Crate"}})["components"]["rigidbody"];
    EXPECT_NEAR(c["friction"].get<float>(), 0.8F, 1e-6F);
    EXPECT_NEAR(c["gravity_factor"].get<float>(), 0.0F, 1e-6F);
    simulate(60);
    const Json after = ok("physics.get", {{"entity", "Crate"}});
    EXPECT_NEAR(after["position"][1].get<float>(), 6.0F, 0.1F) << "gravity factor 0: rises at 2 m/s for 0.5 s";
}

TEST_F(BodyCommands, SetSwitchesMotionType) {
    crate();
    Json s = ok("physics.set", {{"entity", "Crate"}, {"motion_type", "kinematic"}});
    EXPECT_EQ(s["motion_type"], "kinematic");
    EXPECT_EQ(reg().get<engine::scene::RigidBody>(entity(s["entity"])).motion,
              engine::scene::BodyMotion::kinematic);
    // To static: rebuilt from the component on the next frame.
    s = ok("physics.set", {{"entity", "Crate"}, {"motion_type", "static"}});
    EXPECT_TRUE(s["rebuilt"].get<bool>());
    simulate(1);
    EXPECT_EQ(ok("physics.get", {{"entity", "Crate"}})["motion_type"], "static");
}

TEST_F(BodyCommands, ApplyPushesADynamicBody) {
    crate(); // 2 kg
    Json s = ok("physics.apply", {{"entity", "Crate"}, {"impulse", {0, 0, 4}}});
    EXPECT_EQ(s["applied"], "impulse");
    EXPECT_NEAR(s["linear_velocity"][2].get<float>(), 2.0F, 1e-4F);
    s = ok("physics.apply", {{"entity", "Crate"}, {"torque", {0, 10, 0}}, {"duration_s", 0.5}});
    EXPECT_EQ(s["applied"], "torque");
    EXPECT_FLOAT_EQ(s["duration_s"].get<float>(), 0.5F);
    simulate(60);
    EXPECT_GT(ok("physics.get", {{"entity", "Crate"}})["angular_velocity"][1].get<float>(), 1.0F);
}

TEST_F(BodyCommands, ErrorsArePrecise) {
    crate();
    spawn("Wall", {{"collider", {{"shape", "box"}}}, {"rigidbody", {{"motion", "static"}}}});
    spawn("Lamp", {{"point_light", Json::object()}});
    const auto code = rpc_code::invalid_params;
    EXPECT_NE(err("physics.get", {{"entity", "Lamp"}}, code).find("has no rigidbody"),
              std::string::npos);
    EXPECT_NE(err("physics.apply", {{"entity", "Wall"}, {"impulse", {0, 1, 0}}}, code)
                  .find("static"),
              std::string::npos);
    EXPECT_NE(err("physics.apply", {{"entity", "Crate"}}, code).find("exactly one"),
              std::string::npos);
    err("physics.apply", {{"entity", "Crate"}, {"impulse", {0, 1, 0}}, {"force", {1, 0, 0}}},
        code);
    EXPECT_NE(err("physics.apply",
                  {{"entity", "Crate"}, {"angular_impulse", {0, 1, 0}}, {"at_point", {0, 0, 0}}},
                  code)
                  .find("at_point"),
              std::string::npos);
    EXPECT_NE(err("physics.apply", {{"entity", "Crate"}, {"impulse", {0, 1, 0}}, {"duration_s", 1}},
                  code)
                  .find("duration_s"),
              std::string::npos);
    EXPECT_NE(err("physics.apply", {{"entity", "Crate"}, {"impulse", {0, 1e9, 0}}}, code)
                  .find("exceeds"),
              std::string::npos);
    err("physics.set", {{"entity", "Crate"}, {"restitution", 2}}, code);
    err("physics.set", {{"entity", "Crate"}, {"motion_type", "floating"}}, code);
    err("physics.set", {{"entity", "Crate"}, {"linear_velocity", {0, "fast", 0}}}, code);
    EXPECT_NE(err("physics.set", {{"entity", "Wall"}, {"linear_velocity", {1, 0, 0}}}, code)
                  .find("only dynamic bodies"),
              std::string::npos);
    err("physics.set", {{"entity", "Crate"}, {"velocity", {1, 0, 0}}}, code); // unknown field
    err("physics.get", {{"entity", 999999}}, rpc_code::not_found);

    physics.reset();
    EXPECT_NE(err("physics.get", {{"entity", "Crate"}}, rpc_code::unavailable).find("sim.play"),
              std::string::npos);
}

TEST_F(BodyCommands, AnUnbuiltBodyIsReportedAsPending) {
    ok("scene.create", {{"name", "Late"},
                        {"components", {{"collider", {{"shape", "sphere"}}}, {"rigidbody", Json::object()}}}});
    EXPECT_NE(err("physics.get", {{"entity", "Late"}}, rpc_code::unavailable).find("next simulated"),
              std::string::npos);
}

// ---- queries ----------------------------------------------------------------

TEST_F(BodyCommands, RaycastAllReportsEveryEntityAlongTheRay) {
    spawn("Floor", {{"transform", {{"position", {0, -1, 0}}}},
                    {"collider", {{"shape", "box"}, {"half_extents", {20, 1, 20}}}},
                    {"rigidbody", {{"motion", "static"}}}});
    const Json crate_id = crate(); // at y = 5
    const Json r = ok("physics.raycast_all",
                      {{"origin", {0, 10, 0}}, {"direction", {0, -1, 0}}, {"max_distance", 50}});
    ASSERT_EQ(r["count"], 2);
    EXPECT_EQ(r["hits"][0]["entity"], crate_id);
    EXPECT_EQ(r["hits"][0]["name"], "Crate");
    EXPECT_EQ(r["hits"][0]["kind"], "body");
    EXPECT_EQ(r["hits"][0]["layer"], "dynamic");
    EXPECT_NEAR(r["hits"][0]["distance"].get<float>(), 4.5F, 1e-3F);
    EXPECT_EQ(r["hits"][1]["name"], "Floor");
    // Only the static layer.
    const Json s = ok("physics.raycast_all",
                      {{"origin", {0, 10, 0}}, {"direction", {0, -1, 0}}, {"layers", {"static"}}});
    ASSERT_EQ(s["count"], 1);
    EXPECT_EQ(s["hits"][0]["name"], "Floor");
}

TEST_F(BodyCommands, OverlapAndShapeCastFindTheCrate) {
    crate(); // a 1 m box centred at y = 5
    Json o = ok("physics.overlap", {{"shape", "sphere"}, {"radius", 1}, {"position", {0, 6.2, 0}}});
    ASSERT_EQ(o["count"], 1);
    EXPECT_EQ(o["hits"][0]["name"], "Crate");
    EXPECT_GT(o["hits"][0]["penetration"].get<float>(), 0.0F);
    o = ok("physics.overlap", {{"shape", "box"}, {"half_extents", {0.2, 0.2, 0.2}},
                               {"position", {3, 5, 0}}, {"rotation", {0, 45, 0}}});
    EXPECT_EQ(o["count"], 0);

    const Json c = ok("physics.shape_cast", {{"shape", "capsule"},
                                             {"radius", 0.25},
                                             {"half_height", 0.25},
                                             {"from", {-5, 5, 0}},
                                             {"to", {5, 5, 0}}});
    ASSERT_EQ(c["count"], 1);
    EXPECT_EQ(c["hits"][0]["name"], "Crate");
    EXPECT_NEAR(c["hits"][0]["distance"].get<float>(), 4.25F, 0.02F);
    EXPECT_NEAR(c["hits"][0]["normal"][0].get<float>(), -1.0F, 0.01F);
}

TEST_F(BodyCommands, QueryParametersAreValidated) {
    const auto code = rpc_code::invalid_params;
    err("physics.raycast_all", {{"origin", {0, 0, 0}}, {"direction", {0, 0, 0}}}, code);
    err("physics.raycast_all", {{"direction", {0, -1, 0}}}, code);
    EXPECT_NE(err("physics.raycast_all",
                  {{"origin", {0, 0, 0}}, {"direction", {0, -1, 0}}, {"layers", {"water"}}}, code)
                  .find("static, dynamic, character"),
              std::string::npos);
    err("physics.raycast_all", {{"origin", {0, 0, 0}}, {"direction", {0, -1, 0}}, {"max_hits", 0}},
        code);
    EXPECT_NE(err("physics.overlap", {{"shape", "box"}, {"radius", 1}, {"position", {0, 0, 0}}},
                  code)
                  .find("half_extents"),
              std::string::npos);
    err("physics.overlap", {{"shape", "cone"}, {"position", {0, 0, 0}}}, code);
    err("physics.overlap", {{"shape", "sphere"}}, code);
    err("physics.overlap", {{"shape", "sphere"}, {"radius", -1}, {"position", {0, 0, 0}}}, code);
    err("physics.overlap",
        {{"shape", "sphere"}, {"half_height", 1}, {"position", {0, 0, 0}}}, code);
    EXPECT_NE(err("physics.shape_cast",
                  {{"shape", "sphere"}, {"from", {1, 1, 1}}, {"to", {1, 1, 1}}}, code)
                  .find("physics.overlap"),
              std::string::npos);
    err("physics.shape_cast", {{"shape", "sphere"}, {"from", {1, 1, 1}}}, code);
    physics.reset();
    err("physics.raycast_all", {{"origin", {0, 0, 0}}, {"direction", {0, -1, 0}}},
        rpc_code::unavailable);
}

// ---- joints through the component codec ----------------------------------------

TEST_F(BodyCommands, JointsAreAuthoredThroughSceneCommands) {
    crate("Lid");
    const Json box = spawn("Box", {{"transform", {{"position", {0, 4, 0}}}},
                                   {"collider", {{"shape", "box"}}},
                                   {"rigidbody", {{"motion", "static"}}}});
    ok("scene.update", {{"entity", "Lid"},
                        {"components",
                         {{"joint", {{"type", "hinge"},
                                     {"target", "Box"},
                                     {"anchor", {0, -0.5, 0}},
                                     {"axis", {1, 0, 0}},
                                     {"limits", {-110, 0}}}}}}});
    simulate(1);
    EXPECT_EQ(physics->joint_count(), 1U);
    const Json j = ok("scene.get", {{"entity", "Lid"}})["components"]["joint"];
    EXPECT_EQ(j["target"], box);
    EXPECT_TRUE(j["active"].get<bool>());
    // Bad joints are refused atomically with the codec's precise error.
    EXPECT_NE(err("scene.update",
                  {{"entity", "Lid"}, {"components", {{"joint", {{"limits", {10, 20}}}}}}},
                  rpc_code::invalid_params)
                  .find("joint.limits"),
              std::string::npos);
    ok("scene.remove_components", {{"entity", "Lid"}, {"components", {"joint"}}});
    simulate(1);
    EXPECT_EQ(physics->joint_count(), 0U);
}

// ---- gravity ----------------------------------------------------------------

TEST_F(PhysicsCommands, GravityIsASceneSettingEditableWithoutPlaying) {
    Json g = ok("physics.get_gravity");
    EXPECT_NEAR(g["gravity"][1].get<float>(), -9.81F, 1e-5F);
    g = ok("physics.set_gravity", {{"gravity", {0, -1.62, 0}}}); // the Moon, in edit mode
    EXPECT_NEAR(g["magnitude"].get<float>(), 1.62F, 1e-5F);
    EXPECT_NEAR(engine::scene::physics_settings(reg()).gravity.y, -1.62F, 1e-6F);
    EXPECT_NEAR(ok("physics.get_gravity")["gravity"][1].get<float>(), -1.62F, 1e-5F);
}

TEST_F(PhysicsCommands, GravityReachesARunningWorldAtOnce) {
    auto w = PhysicsWorld::create();
    ASSERT_OK(w);
    physics.emplace(std::move(*w));
    ok("physics.set_gravity", {{"gravity", {0, 3, 0}}});
    EXPECT_EQ(physics->gravity(), glm::vec3(0.0F, 3.0F, 0.0F));
}

TEST_F(PhysicsCommands, GravityIsValidated) {
    const auto code = rpc_code::invalid_params;
    EXPECT_NE(err("physics.set_gravity", Json::object(), code).find("required"),
              std::string::npos);
    EXPECT_NE(err("physics.set_gravity", {{"gravity", {0, -5000, 0}}}, code).find("exceeds"),
              std::string::npos);
    err("physics.set_gravity", {{"gravity", {0, -9.81}}}, code);
    err("physics.set_gravity", {{"x", 0}, {"y", -9.81}, {"z", 0}}, code);
    err("physics.get_gravity", {{"extra", 1}}, code);
    EXPECT_NEAR(engine::scene::physics_settings(reg()).gravity.y, -9.81F, 1e-6F);
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
