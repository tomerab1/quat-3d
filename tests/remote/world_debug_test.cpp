// Debugging commands (ROADMAP A5): debug.log, debug.stats, debug.pause /
// resume / time_scale / step, debug.watch, debug.raycast, debug.nav_path —
// through RpcServer::call with fake services (debug.step, which is deferred,
// over a loopback socket).

#include <chrono>
#include <cmath>
#include <functional>
#include <optional>
#include <string>
#include <thread>
#include <utility>
#include <vector>

#include <glm/glm.hpp>
#include <gtest/gtest.h>

#include "engine/core/log.hpp"
#include "engine/core/sim_clock.hpp"
#include "engine/nav/navmesh.hpp"
#include "engine/physics/physics_world.hpp"
#include "nav/nav_fixtures.hpp"
#include "remote/socket.hpp"
#include "remote/world_fixture.hpp"
#include "support/expect_ok.hpp"

namespace {

using quat::test::Json;
namespace code = engine::remote::rpc_code;
namespace core = engine::core;
using engine::physics::PhysicsWorld;

bool contains(const std::string& haystack, const std::string& needle) {
    return haystack.find(needle) != std::string::npos;
}

// ---- No services: everything app-side answers `unavailable` -------------------

class DebugNoServices : public quat::test::WorldCommands {
protected:
    void SetUp() override { register_commands(); }
};

TEST_F(DebugNoServices, AppCapabilitiesAreUnavailable) {
    err("debug.log", Json::object(), code::unavailable);
    err("debug.pause", Json::object(), code::unavailable);
    err("debug.resume", Json::object(), code::unavailable);
    err("debug.time_scale", {{"scale", 2}}, code::unavailable);
    err("debug.raycast", {{"origin", {0, 1, 0}}, {"direction", {0, -1, 0}}}, code::unavailable);
    err("debug.nav_path", {{"from", {0, 0, 0}}, {"to", {1, 0, 1}}}, code::unavailable);
    // Deferred: cannot complete inside call() (or a batch).
    err("debug.step", Json::object(), code::unavailable);
    // Parameters are still validated first.
    err("debug.log", {{"level", "loud"}}, code::invalid_params);
    err("debug.time_scale", {{"scale", 100}}, code::invalid_params);

    // debug.stats still reports what the scene itself knows.
    (void)scene.create_entity("a");
    const Json s = ok("debug.stats");
    EXPECT_EQ(s["entities"], 1);
    EXPECT_TRUE(s["physics"].is_null());
    EXPECT_TRUE(s["sim"].is_null());
    EXPECT_TRUE(s["frame_time_ms"].is_null());
    EXPECT_EQ(s["nav"]["agents"], 0);
    EXPECT_TRUE(s["nav"]["navmesh_polygons"].is_null());
}

// ---- With services ---------------------------------------------------------------

class DebugCommands : public quat::test::WorldCommands {
protected:
    core::LogRing                ring{64};
    core::SimClock               clock;
    std::optional<PhysicsWorld>  physics;
    engine::nav::NavMesh         navmesh;
    engine::remote::FrameStats   frame{};

    void SetUp() override {
        services.log_ring = &ring;
        services.sim_clock = &clock;
        services.frame_stats = [this] { return frame; };
        services.physics_world = [this]() -> const PhysicsWorld* {
            return physics ? &*physics : nullptr;
        };
        services.navmesh = [this]() -> const engine::nav::NavMesh* { return &navmesh; };
        register_commands();
    }

    entt::entity create(const Json& params) {
        return entity(ok("scene.create", params)["id"]);
    }
};

TEST_F(DebugCommands, LogTailsAndFilters) {
    ring.push(core::LogLevel::info, "nav", "baked 12 polygons");
    ring.push(core::LogLevel::warn, "stderr", "primitive resolve: no mesh");
    ring.push(core::LogLevel::error, "vulkan", "VUID-vkCmdDraw-None-02699");

    Json r = ok("debug.log");
    ASSERT_EQ(r["lines"].size(), 3U);
    EXPECT_EQ(r["lines"][0]["seq"], 1);
    EXPECT_EQ(r["lines"][2]["level"], "error");
    EXPECT_EQ(r["lines"][2]["category"], "vulkan");
    EXPECT_TRUE(r["lines"][0]["t"].is_number());
    EXPECT_EQ(r["cursor"], 3);
    EXPECT_EQ(r["more"], false);
    EXPECT_EQ(r["capacity"], 64);

    r = ok("debug.log", {{"level", "warn"}});
    ASSERT_EQ(r["lines"].size(), 2U);
    r = ok("debug.log", {{"category", "vulkan"}});
    ASSERT_EQ(r["lines"].size(), 1U);
    r = ok("debug.log", {{"contains", "polygons"}});
    ASSERT_EQ(r["lines"].size(), 1U);

    // Incremental: only what came after the cursor.
    ring.push(core::LogLevel::info, "remote", "spawned x");
    r = ok("debug.log", {{"since_seq", 3}});
    ASSERT_EQ(r["lines"].size(), 1U);
    EXPECT_EQ(r["lines"][0]["message"], "spawned x");
    EXPECT_EQ(r["cursor"], 4);
    r = ok("debug.log", {{"since_seq", 0}, {"limit", 2}});
    EXPECT_EQ(r["lines"].size(), 2U);
    EXPECT_EQ(r["more"], true);
    EXPECT_EQ(r["cursor"], 2);

    EXPECT_TRUE(contains(err("debug.log", {{"level", "loud"}}, code::invalid_params), "level"));
    EXPECT_TRUE(contains(err("debug.log", {{"limit", 0}}, code::invalid_params), "limit"));
    EXPECT_TRUE(contains(err("debug.log", {{"since", 1}}, code::invalid_params), "since_seq"))
        << "unknown fields name the known ones";
}

TEST_F(DebugCommands, LogReportsMissedLinesAfterWraparound) {
    for (int i = 0; i < 100; ++i) ring.push(core::LogLevel::info, "t", std::to_string(i));
    const Json r = ok("debug.log", {{"since_seq", 10}, {"limit", 1000}});
    EXPECT_EQ(r["missed"], 100 - 64 - 10);
    EXPECT_EQ(r["lines"].size(), 64U);
    EXPECT_EQ(r["oldest_seq"], 37);
}

TEST_F(DebugCommands, StatsReportCountersFromEveryService) {
    frame.frame = 500;
    frame.last_ms = 20.0F;
    frame.avg_ms = 16.0F;
    frame.worst_ms = 40.0F;
    frame.samples = 240;
    frame.draws = 37;
    frame.resident_bytes = 1234567;
    ring.push(core::LogLevel::warn, "a", "w");
    ring.push(core::LogLevel::error, "a", "e");
    auto physics_world = PhysicsWorld::create();
    ASSERT_OK(physics_world);
    physics = std::move(*physics_world);
    (void)physics->add_body({.shape = physics->create_box(glm::vec3(1.0F))});
    (void)physics->create_character(0.5F, 0.3F, glm::vec3(5.0F, 0.0F, 0.0F));
    const entt::entity agent = create({{"name", "npc"}});
    reg().emplace<engine::scene::NavAgent>(agent).active = true;

    const Json s = ok("debug.stats");
    EXPECT_EQ(s["frame"], 500);
    EXPECT_FLOAT_EQ(s["frame_time_ms"]["avg"].get<float>(), 16.0F);
    EXPECT_FLOAT_EQ(s["frame_time_ms"]["worst"].get<float>(), 40.0F);
    EXPECT_FLOAT_EQ(s["fps"].get<float>(), 62.5F);
    EXPECT_EQ(s["draws"], 37);
    EXPECT_EQ(s["memory"]["resident_bytes"], 1234567);
    EXPECT_EQ(s["entities"], 1);
    EXPECT_EQ(s["physics"]["bodies"], 1);
    EXPECT_EQ(s["physics"]["characters"], 1);
    EXPECT_EQ(s["nav"]["agents"], 1);
    EXPECT_EQ(s["nav"]["moving"], 1);
    EXPECT_TRUE(s["nav"]["navmesh_polygons"].is_null()) << "nothing baked";
    EXPECT_EQ(s["sim"]["paused"], false);
    EXPECT_EQ(s["log"]["warnings"], 1);
    EXPECT_EQ(s["log"]["errors"], 1);
    EXPECT_EQ(s["log"]["last_error_seq"], 2);
    err("debug.stats", {{"window", 10}}, code::invalid_params);
}

TEST_F(DebugCommands, PauseResumeAndTimeScaleDriveTheClock) {
    Json s = ok("debug.pause");
    EXPECT_EQ(s["paused"], true);
    EXPECT_TRUE(clock.state().paused);
    EXPECT_EQ(clock.advance(0.016F), 0.0F);

    s = ok("debug.time_scale", {{"scale", 0.25}});
    EXPECT_FLOAT_EQ(s["time_scale"].get<float>(), 0.25F);
    s = ok("debug.resume");
    EXPECT_EQ(s["paused"], false);
    EXPECT_FLOAT_EQ(clock.advance(0.02F), 0.005F);
    EXPECT_EQ(s.size(), 6U) << s.dump();

    EXPECT_TRUE(contains(err("debug.time_scale", Json::object(), code::invalid_params), "scale"));
    EXPECT_TRUE(contains(err("debug.time_scale", {{"scale", 0}}, code::invalid_params), "scale"));
    err("debug.time_scale", {{"scale", "fast"}}, code::invalid_params);
    err("debug.pause", {{"now", true}}, code::invalid_params);
}

TEST_F(DebugCommands, WatchSnapshotsSelectedComponents) {
    const entt::entity e = create(
        {{"name", "crate"},
         {"components",
          {{"transform", {{"position", {1, 2, 3}}}},
           {"primitive", {{"shape", "cube"}}},
           {"nav_agent", {{"target", {4, 0, 4}}, {"speed", 2}}}}}});
    reg().get<engine::scene::NavAgent>(e).failed = true;
    (void)clock.advance(0.5F);
    frame.frame = 42;

    Json r = ok("debug.watch", {{"entity", "crate"}, {"components", {"transform", "rigidbody"}}});
    ASSERT_EQ(r["entities"].size(), 1U);
    const Json& w = r["entities"][0];
    EXPECT_EQ(w["name"], "crate");
    EXPECT_EQ(w["world_position"], Json({1.0, 2.0, 3.0}));
    EXPECT_TRUE(w["components"].contains("transform"));
    EXPECT_FALSE(w["components"].contains("primitive"));
    EXPECT_EQ(w["missing"], Json({"rigidbody"}));
    EXPECT_FALSE(w.contains("nav_agent_state")) << "nav_agent was not asked for";
    EXPECT_EQ(r["frame"], 42);
    EXPECT_NEAR(r["sim_time_s"].get<double>(), 0.5, 1e-6);

    r = ok("debug.watch", {{"entities", {entt::to_integral(e)}}});
    const Json& all = r["entities"][0];
    EXPECT_TRUE(all["components"].contains("primitive"));
    ASSERT_TRUE(all.contains("nav_agent_state"));
    EXPECT_EQ(all["nav_agent_state"]["failed"], true);
    EXPECT_TRUE(all["nav_agent_state"]["path"].is_array());

    EXPECT_TRUE(contains(err("debug.watch", {{"entity", "crate"}, {"components", {"gravity"}}},
                             code::invalid_params),
                         "known: "));
    err("debug.watch", Json::object(), code::invalid_params);
    err("debug.watch", {{"entity", "crate"}, {"entities", {1}}}, code::invalid_params);
    err("debug.watch", {{"entities", Json::array()}}, code::invalid_params);
    err("debug.watch", {{"entity", "ghost"}}, code::not_found);
    err("debug.watch", {{"entities", {entt::to_integral(e), 999999}}}, code::not_found);
}

TEST_F(DebugCommands, RaycastHitsBodiesAndCharactersAndNamesTheEntity) {
    err("debug.raycast", {{"origin", {0, 1, 0}}, {"direction", {0, 0, -1}}}, code::unavailable);
    auto world = PhysicsWorld::create();
    ASSERT_OK(world);
    physics = std::move(*world);

    const entt::entity wall = create({{"name", "wall"}});
    PhysicsWorld::BodyParams wall_body;
    wall_body.shape = physics->create_box(glm::vec3(2.0F, 2.0F, 0.5F));
    wall_body.position = glm::vec3(0.0F, 0.0F, -5.0F);
    wall_body.motion = engine::physics::Motion::static_body;
    wall_body.layer = engine::physics::Layer::static_body;
    wall_body.owner = static_cast<std::uint32_t>(entt::to_integral(wall));
    ASSERT_NE(physics->add_body(wall_body), PhysicsWorld::invalid_body);

    PhysicsWorld::BodyParams trigger;
    trigger.shape = physics->create_box(glm::vec3(1.0F));
    trigger.position = glm::vec3(0.0F, 0.0F, -2.0F);
    trigger.motion = engine::physics::Motion::static_body;
    trigger.layer = engine::physics::Layer::trigger;
    trigger.sensor = true;
    ASSERT_NE(physics->add_body(trigger), PhysicsWorld::invalid_body);

    Json r = ok("debug.raycast", {{"origin", {0, 0, 0}}, {"direction", {0, 0, -2}}});
    ASSERT_EQ(r["hit"], true) << r.dump();
    EXPECT_EQ(r["entity"], entt::to_integral(wall));
    EXPECT_EQ(r["name"], "wall");
    EXPECT_EQ(r["kind"], "body");
    EXPECT_NEAR(r["distance"].get<float>(), 4.5F, 1e-3F) << "the sensor in front is skipped";
    EXPECT_NEAR(r["point"][2].get<float>(), -4.5F, 1e-3F);
    EXPECT_NEAR(r["normal"][2].get<float>(), 1.0F, 1e-3F);

    r = ok("debug.raycast",
           {{"origin", {0, 0, 0}}, {"direction", {0, 0, -1}}, {"include_sensors", true}});
    EXPECT_NEAR(r["distance"].get<float>(), 1.0F, 1e-3F);
    EXPECT_EQ(r["sensor"], true);
    EXPECT_TRUE(r["entity"].is_null());
    EXPECT_EQ(r["kind"], "static_world") << "an ownerless body";

    r = ok("debug.raycast", {{"origin", {0, 0, 0}}, {"direction", {0, 0, -1}}, {"max_distance", 3}});
    EXPECT_EQ(r["hit"], false);
    r = ok("debug.raycast", {{"origin", {0, 0, 0}}, {"direction", {0, 1, 0}}});
    EXPECT_EQ(r["hit"], false);

    // A character capsule in the way (characters are not Jolt bodies).
    const entt::entity hero = create({{"name", "hero"}});
    (void)physics->create_character(0.5F, 0.4F, glm::vec3(3.0F, 0.0F, 0.0F),
                                    static_cast<std::uint32_t>(entt::to_integral(hero)));
    r = ok("debug.raycast", {{"origin", {0, 0, 0}}, {"direction", {1, 0, 0}}});
    ASSERT_EQ(r["hit"], true) << r.dump();
    EXPECT_EQ(r["kind"], "character");
    EXPECT_EQ(r["name"], "hero");
    EXPECT_NEAR(r["distance"].get<float>(), 2.6F, 1e-2F);
    EXPECT_NEAR(r["normal"][0].get<float>(), -1.0F, 1e-2F);

    err("debug.raycast", {{"origin", {0, 0, 0}}}, code::invalid_params);
    err("debug.raycast", {{"origin", {0, 0, 0}}, {"direction", {0, 0, 0}}}, code::invalid_params);
    err("debug.raycast", {{"direction", {0, 0, 1}}}, code::invalid_params);
    err("debug.raycast", {{"origin", {0, 0, 0}}, {"direction", {0, 0, 1}}, {"max_distance", -1}},
        code::invalid_params);
}

TEST_F(DebugCommands, NavPathQueriesTheBakedNavmesh) {
    EXPECT_TRUE(contains(err("debug.nav_path", {{"from", {0, 0, 0}}, {"to", {5, 0, 5}}},
                             code::unavailable),
                         "nav.bake"));
    entt::registry geometry;
    (void)quat::test::add_box(geometry, glm::vec3(0.0F, -0.5F, 0.0F), glm::vec3(10.0F, 0.5F, 10.0F));
    auto baked = quat::test::bake(geometry);
    ASSERT_OK(baked);
    navmesh = std::move(*baked);

    Json r = ok("debug.nav_path", {{"from", {-5, 0, -5}}, {"to", {5, 0, 5}}});
    ASSERT_GE(r["points"].size(), 2U) << r.dump();
    EXPECT_EQ(r["partial"], false);
    EXPECT_NEAR(r["length"].get<float>(), std::sqrt(200.0F), 0.5F);
    const Json& last = r["points"].back();
    EXPECT_NEAR(last[0].get<float>(), 5.0F, 0.5F);
    EXPECT_NEAR(last[2].get<float>(), 5.0F, 0.5F);

    // The stats see the bake too.
    EXPECT_GT(ok("debug.stats")["nav"]["navmesh_polygons"].get<int>(), 0);

    err("debug.nav_path", {{"from", {500, 0, 500}}, {"to", {5, 0, 5}}}, code::failed);
    err("debug.nav_path", {{"from", {0, 0, 0}}}, code::invalid_params);
    err("debug.nav_path", {{"from", {0, 0}}, {"to", {1, 0, 1}}}, code::invalid_params);
}

// ---- debug.step (deferred) over a real connection ----------------------------------

class DebugStep : public ::testing::Test {
protected:
    engine::scene::Scene                     scene;
    std::optional<engine::remote::RpcServer> server;
    core::SimClock                           clock;
    std::vector<std::pair<std::uint64_t, std::function<void()>>> waiters;
    engine::remote::detail::Socket           sock;
    std::string                              inbuf;

    void SetUp() override {
        engine::remote::RpcServerConfig config;
        config.port = 0;
        auto listened = engine::remote::RpcServer::listen(config);
        ASSERT_OK(listened);
        server.emplace(std::move(*listened));
        engine::remote::WorldServices s;
        s.sim_clock = &clock;
        s.when_stepped = [this](std::uint64_t target, std::function<void()> done) {
            waiters.emplace_back(target, std::move(done));
        };
        engine::remote::register_world_commands(*server, scene, std::move(s));
        auto c = engine::remote::detail::connect_loopback(server->port());
        ASSERT_OK(c);
        sock = std::move(*c);
    }

    void send(const Json& msg) {
        const std::string line = msg.dump() + "\n";
        std::size_t off = 0;
        for (int spins = 0; off < line.size() && spins < 1000; ++spins) {
            const auto r = engine::remote::detail::send_some(sock, line.data() + off, line.size() - off);
            if (r.status == engine::remote::detail::IoStatus::ok) off += r.bytes;
        }
        ASSERT_EQ(off, line.size());
    }

    // Polls the server for up to `ms` milliseconds; returns a reply or null.
    Json pump(int ms) {
        const auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(ms);
        while (std::chrono::steady_clock::now() < deadline) {
            if (const auto nl = inbuf.find('\n'); nl != std::string::npos) {
                const std::string line = inbuf.substr(0, nl);
                inbuf.erase(0, nl + 1);
                return Json::parse(line, nullptr, false);
            }
            server->poll();
            char buf[4096];
            const auto r = engine::remote::detail::recv_some(sock, buf, sizeof(buf));
            if (r.status == engine::remote::detail::IoStatus::ok) {
                inbuf.append(buf, r.bytes);
            } else {
                std::this_thread::sleep_for(std::chrono::milliseconds(1));
            }
        }
        return nullptr;
    }
};

TEST_F(DebugStep, RepliesOnceTheStepsHaveRun) {
    send({{"jsonrpc", "2.0"}, {"id", 1}, {"method", "debug.step"}, {"params", {{"frames", 3}}}});
    EXPECT_TRUE(pump(100).is_null()) << "replied before the steps ran";
    ASSERT_EQ(waiters.size(), 1U);
    EXPECT_EQ(waiters[0].first, 3U);
    EXPECT_TRUE(clock.state().paused);
    EXPECT_EQ(clock.state().pending_steps, 3U);

    // The frame loop runs the steps; the application then completes the wait.
    for (int i = 0; i < 3; ++i) EXPECT_FLOAT_EQ(clock.advance(0.1F), core::SimClock::default_step_dt);
    waiters[0].second();
    const Json reply = pump(2000);
    ASSERT_TRUE(reply.is_object()) << "no reply";
    EXPECT_EQ(reply["id"], 1);
    ASSERT_TRUE(reply.contains("result")) << reply.dump();
    EXPECT_EQ(reply["result"]["steps_run"], 3);
    EXPECT_EQ(reply["result"]["pending_steps"], 0);
    EXPECT_EQ(reply["result"]["paused"], true);
}

TEST_F(DebugStep, BadParametersAnswerImmediately) {
    send({{"jsonrpc", "2.0"}, {"id", 2}, {"method", "debug.step"}, {"params", {{"frames", 0}}}});
    const Json reply = pump(2000);
    ASSERT_TRUE(reply.is_object());
    EXPECT_EQ(reply["error"]["code"], code::invalid_params);
    EXPECT_TRUE(contains(reply["error"]["message"].get<std::string>(), "frames"));
    EXPECT_TRUE(waiters.empty());
    EXPECT_FALSE(clock.state().paused) << "a rejected step must not pause";
}

TEST_F(DebugStep, DroppedWaitStillAnswers) {
    send({{"jsonrpc", "2.0"}, {"id", 3}, {"method", "debug.step"}, {"params", {{"dt", 0.05}}}});
    (void)pump(50);
    ASSERT_EQ(waiters.size(), 1U);
    waiters.clear(); // the application went away without completing it
    const Json reply = pump(2000);
    ASSERT_TRUE(reply.is_object());
    EXPECT_EQ(reply["id"], 3);
    EXPECT_TRUE(reply.contains("error")) << reply.dump();
}

TEST_F(DebugStep, LaterRequestsKeepQueuedStepsDt) {
    send({{"jsonrpc", "2.0"}, {"id", 4}, {"method", "debug.step"},
          {"params", {{"frames", 2}, {"dt", 0.1}}}});
    send({{"jsonrpc", "2.0"}, {"id", 5}, {"method", "debug.step"},
          {"params", {{"frames", 1}, {"dt", 0.02}}}});
    EXPECT_TRUE(pump(100).is_null());
    ASSERT_EQ(waiters.size(), 2U);
    EXPECT_EQ(waiters[0].first, 2U);
    EXPECT_EQ(waiters[1].first, 3U);
    EXPECT_FLOAT_EQ(clock.advance(0.5F), 0.1F);
    EXPECT_FLOAT_EQ(clock.advance(0.5F), 0.1F);
    EXPECT_FLOAT_EQ(clock.advance(0.5F), 0.02F);
}

TEST_F(DebugStep, WaitingRequestsAreCapped) {
    constexpr int k_cap = 64;
    for (int i = 0; i < k_cap; ++i) {
        send({{"jsonrpc", "2.0"}, {"id", 100 + i}, {"method", "debug.step"},
              {"params", Json::object()}});
    }
    EXPECT_TRUE(pump(200).is_null()) << "all within the cap wait";
    ASSERT_EQ(waiters.size(), static_cast<std::size_t>(k_cap));

    send({{"jsonrpc", "2.0"}, {"id", 1}, {"method", "debug.step"}, {"params", Json::object()}});
    Json reply = pump(2000);
    ASSERT_TRUE(reply.is_object()) << "no reply";
    EXPECT_EQ(reply["id"], 1);
    ASSERT_TRUE(reply.contains("error")) << reply.dump();
    EXPECT_TRUE(contains(reply["error"]["message"].get<std::string>(), "busy"));
    EXPECT_EQ(clock.state().pending_steps, static_cast<std::uint32_t>(k_cap))
        << "a refused request queues no step";

    // Once one completes there is room again.
    waiters[0].second();
    reply = pump(2000);
    ASSERT_TRUE(reply.is_object());
    EXPECT_EQ(reply["id"], 100);
    send({{"jsonrpc", "2.0"}, {"id", 2}, {"method", "debug.step"}, {"params", Json::object()}});
    EXPECT_TRUE(pump(100).is_null());
    EXPECT_EQ(waiters.size(), static_cast<std::size_t>(k_cap) + 1);
}

} // namespace

