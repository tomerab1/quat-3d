// G7: trigger callbacks from a real PhysicsWorld — a dynamic body and a
// character controller entering a sensor — plus impulses and raycasts.

#include <glm/gtc/matrix_transform.hpp>

#include "engine/physics/physics_world.hpp"
#include "script/script_fixture.hpp"

namespace quat::test {
namespace {

using namespace engine::scene;
using engine::physics::PhysicsWorld;

class ScriptPhysicsTest : public ScriptTest {
protected:
    std::optional<PhysicsWorld> world;

    void start_physics() {
        auto w = PhysicsWorld::create();
        ASSERT_OK(w);
        world.emplace(std::move(*w));
        start();
        engine::script::ScriptServices services;
        services.physics = &*world;
        runtime->set_services(services);
    }

    entt::entity box(const std::string& name, const glm::vec3& pos, const glm::vec3& half,
                     BodyMotion motion, bool sensor) {
        const entt::entity e = scene.create_entity(name);
        scene.registry().get<Transform>(e).local = glm::translate(glm::mat4(1.0F), pos);
        Collider c;
        c.half_extents = half;
        c.is_sensor = sensor;
        scene.registry().emplace<Collider>(e, c);
        RigidBody rb;
        rb.motion = motion;
        scene.registry().emplace<RigidBody>(e, rb);
        return e;
    }

    // One frame in the application's order: simulation, triggers, scripts.
    void step(int n = 1) {
        for (int i = 0; i < n; ++i) {
            const float dt = 1.0F / 60.0F;
            scene.tick(0.0F);
            character_system(scene.registry(), *world, dt);
            physics_system(scene.registry(), *world, dt);
            runtime->dispatch_triggers(*world);
            runtime->update(dt);
        }
    }
};

TEST_F(ScriptPhysicsTest, DynamicBodyEntersAndLeavesASensor) {
    write("zone.lua", R"(
        function on_trigger_enter(self, other) print("zone enter", other:name()) end
        function on_trigger_exit(self, other) print("zone exit", other:name()) end
    )");
    write("ball.lua", R"(
        function on_trigger_enter(self, other) print("ball entered", other:name()) end
    )");
    const entt::entity zone = box("zone", {0, 3, 0}, glm::vec3(1.0F), BodyMotion::static_body, true);
    const entt::entity ball = box("ball", {0, 6, 0}, glm::vec3(0.25F), BodyMotion::dynamic, false);
    scene.registry().emplace<Script>(zone, script("zone.lua"));
    scene.registry().emplace<Script>(ball, script("ball.lua"));
    start_physics();
    step(150);
    const auto log = script_log();
    EXPECT_TRUE(has_line(log, "zone enter\tball")) << ::testing::PrintToString(log);
    EXPECT_TRUE(has_line(log, "ball entered\tzone"));
    EXPECT_TRUE(has_line(log, "zone exit\tball"));
}

TEST_F(ScriptPhysicsTest, CharacterCollectsACoin) {
    write("coin.lua", R"(
        function on_trigger_enter(self, other)
            if other:has("character") then
                print("collected by", other:name())
                self.entity:destroy()
            end
        end
    )");
    write("player.lua", R"(
        function on_update(self, dt) self.entity:move(vec3(1, 0, 0)) end
    )");
    const entt::entity coin = box("coin", {2, 0.9F, 0}, glm::vec3(0.3F), BodyMotion::static_body, true);
    scene.registry().emplace<Script>(coin, script("coin.lua"));
    (void)box("floor", {0, -0.5F, 0}, glm::vec3(20.0F, 0.5F, 20.0F), BodyMotion::static_body, false);
    const entt::entity player = scene.create_entity("player");
    scene.registry().get<Transform>(player).local = glm::translate(glm::mat4(1.0F), {0, 0.9F, 0});
    scene.registry().emplace<CharacterController>(player);
    scene.registry().emplace<Script>(player, script("player.lua"));
    start_physics();
    step(120);
    const auto log = script_log();
    EXPECT_TRUE(has_line(log, "collected by\tplayer")) << ::testing::PrintToString(log);
    EXPECT_FALSE(scene.registry().valid(coin)) << "the coin destroyed itself";
}

TEST_F(ScriptPhysicsTest, ImpulsesAndRaycasts) {
    write("kick.lua", R"(
        function on_start(self)
            local crate = scene.spawn({
                name = "crate",
                transform = { position = vec3(0, 0.5, 0) },
                collider = { shape = "box", half_extents = vec3(0.5) },
                rigidbody = { motion = "dynamic", mass = 1 },
            })
            crate:add_impulse(vec3(0, 0, -5)) -- the body exists immediately
            self.crate = crate
            local hit = physics.raycast(vec3(0, 10, 0), vec3(0, -1, 0))
            print("hit", hit and hit.entity:name(), hit and math.floor(hit.distance * 10 + 0.5))
            print("miss", physics.raycast(vec3(100, 10, 0), vec3(0, -1, 0)))
        end
        function on_update(self, dt)
            if time.frame() == 30 then print("moved", self.crate:position().z < -1) end
        end
    )");
    (void)box("floor", {0, -0.5F, 0}, glm::vec3(20.0F, 0.5F, 20.0F), BodyMotion::static_body, false);
    scripted("kicker", "kick.lua");
    start_physics();
    step(30);
    const auto log = script_log();
    EXPECT_TRUE(has_line(log, "hit\tcrate\t90")) << ::testing::PrintToString(log);
    EXPECT_TRUE(has_line(log, "miss\tnil"));
    EXPECT_TRUE(has_line(log, "moved\ttrue"));
}

} // namespace
} // namespace quat::test
