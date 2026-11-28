// G7: the entity / scene API — transforms, component get/set through the
// codec, spawn / clone, math types and input.

#include <glm/gtc/matrix_transform.hpp>

#include <nlohmann/json.hpp>

#include "engine/input/actions.hpp"
#include "engine/scene/component_json.hpp"
#include "engine/script/script_input.hpp"
#include "script/script_fixture.hpp"

namespace quat::test {
namespace {

using namespace engine::scene;

TEST_F(ScriptTest, ComponentsRoundTripThroughTheCodec) {
    write("codec.lua", R"(
        function on_start(self)
            local e = self.entity
            e:set("point_light", { color = vec3(1, 0.5, 0.25), radius = 4, intensity = 2 })
            local light = e:get("point_light")
            print("light", light.radius, light.intensity, tostring(light.color))
            print("has", e:has("point_light"), e:has("camera"))
            e:set("point_light", { radius = 9 })
            print("merged", e:get("point_light").radius, e:get("point_light").intensity)
            local ok, err = pcall(e.set, e, "point_light", { radius = "far" })
            print("bad", ok, err)
            ok, err = pcall(e.set, e, "point_light", { radius = 1, colour = vec3(1) })
            print("unknown", ok, err)
            print("removed", e:remove("point_light"), e:has("point_light"))
            print("camera", e:get("camera"))
        end
    )");
    const entt::entity e = scripted("lamp", "codec.lua");
    start();
    frames();
    const auto log = script_log();
    EXPECT_TRUE(has_line(log, "light\t4.0\t2.0\tvec3(1, 0.5, 0.25)")) << ::testing::PrintToString(log);
    EXPECT_TRUE(has_line(log, "has\ttrue\tfalse"));
    EXPECT_TRUE(has_line(log, "merged\t9.0\t2.0"));
    // Called through pcall (a C function), so no script position is prefixed.
    EXPECT_TRUE(has_line(log, "bad\tfalse\tpoint_light.radius: expected"));
    EXPECT_TRUE(has_line(log, "unknown\tfalse")) << ::testing::PrintToString(log);
    EXPECT_TRUE(has_line(log, "colour: unknown field"));
    EXPECT_TRUE(has_line(log, "removed\ttrue\tfalse"));
    EXPECT_TRUE(has_line(log, "camera\tnil"));
    EXPECT_FALSE(scene.registry().all_of<PointLight>(e));
}

TEST_F(ScriptTest, TransformsAndHierarchy) {
    write("move.lua", R"(
        function on_start(self)
            local e = self.entity
            e:set_position(vec3(1, 2, 3))
            print("pos", tostring(e:position()))
            e:set_rotation(quat.euler(vec3(0, 90, 0)))
            print("fwd", tostring(e:forward()))
            local child = scene.spawn({ name = "kid", parent = e, transform = { position = vec3(0, 1, 0) } })
            print("child", tostring(child:position()), child:parent() == e, #e:children())
            e:translate(vec3(10, 0, 0))
            print("child moved", tostring(child:position()))
            e:set_scale(2)
            print("scale", tostring(e:scale()))
        end
    )");
    scripted("mover", "move.lua");
    start();
    frames();
    const auto log = script_log();
    EXPECT_TRUE(has_line(log, "pos\tvec3(1, 2, 3)")) << ::testing::PrintToString(log);
    EXPECT_TRUE(has_line(log, "fwd\tvec3(-1, ")) << "yaw 90 turns forward (-Z) to -X";
    EXPECT_TRUE(has_line(log, "child\tvec3(1, 3, 3)\ttrue\t1"));
    EXPECT_TRUE(has_line(log, "child moved\tvec3(11, 3, 3)"));
    EXPECT_TRUE(has_line(log, "scale\tvec3(2, 2, 2)"));
}

TEST_F(ScriptTest, SpawnCloneAndFind) {
    write("spawner.lua", R"(
        function on_start(self)
            local coin = scene.spawn({
                name = "coin",
                transform = { position = vec3(0, 1, 0) },
                primitive = { shape = "sphere", color = "#ffd700" },
                collider = { shape = "sphere", half_extents = vec3(0.5), sensor = true },
            })
            local copy = scene.clone(coin)
            copy:set_position(vec3(5, 1, 0))
            print("found", #scene.find_all("coin"), scene.find("nobody"))
            print("prims", #scene.with_component("primitive"))
            local ok, err = pcall(scene.spawn, { name = "bad", primitve = {} })
            print("typo", ok, err)
        end
    )");
    scripted("spawner", "spawner.lua");
    start();
    frames();
    const auto log = script_log();
    EXPECT_TRUE(has_line(log, "found\t2\tnil")) << ::testing::PrintToString(log);
    EXPECT_TRUE(has_line(log, "prims\t2"));
    EXPECT_TRUE(has_line(log, "typo\tfalse")) << ::testing::PrintToString(log);
    EXPECT_TRUE(has_line(log, "unknown component 'primitve'"));
    int coins = 0;
    for (const auto [e, n] : scene.registry().view<Name>().each()) {
        if (n.value != "coin") continue;
        ++coins;
        EXPECT_TRUE(scene.registry().all_of<Primitive>(e));
        EXPECT_TRUE(scene.registry().get<Collider>(e).is_sensor);
    }
    EXPECT_EQ(coins, 2);
    // The failed spawn left nothing behind.
    for (const auto [e, n] : scene.registry().view<Name>().each()) EXPECT_NE(n.value, "bad");
}

TEST_F(ScriptTest, MathTypes) {
    write("math.lua", R"(
        function on_start(self)
            local a = vec3(1, 2, 3)
            local b = a * 2 + vec3(1)
            print("vec", tostring(b), b:length() > 7, a:dot(vec3(1, 0, 0)), tostring(-a))
            print("cross", tostring(vec3(1, 0, 0):cross(vec3(0, 1, 0))))
            local q = quat.angle_axis(90, vec3(0, 1, 0))
            local r = q * vec3(1, 0, 0)
            print("rot", math.abs(r.x) < 1e-5, math.abs(r.z + 1) < 1e-5)
            print("euler", math.floor(q:to_euler().y + 0.5))
            print("bad", pcall(function() return a + 1 end))
        end
    )");
    scripted("m", "math.lua");
    start();
    frames();
    const auto log = script_log();
    EXPECT_TRUE(has_line(log, "vec\tvec3(3, 5, 7)\ttrue\t1.0\tvec3(-1, -2, -3)")) << ::testing::PrintToString(log);
    EXPECT_TRUE(has_line(log, "cross\tvec3(0, 0, 1)"));
    EXPECT_TRUE(has_line(log, "rot\ttrue\ttrue"));
    EXPECT_TRUE(has_line(log, "euler\t90"));
    EXPECT_TRUE(has_line(log, "bad\tfalse"));
}

TEST_F(ScriptTest, InputReadsTheSampledState) {
    write("input.lua", R"(
        function on_update(self, dt)
            print("keys", input.key("w"), input.key_pressed("space"), input.action_pressed("jump"))
            print("move", tostring(input.move_vector()), input.axis("move_x"))
            print("unknown", pcall(input.key, "hyperspace"))
        end
    )");
    scripted("i", "input.lua");
    engine::script::ScriptInput in;
    in.keys.set(*engine::script::key_from_name("w"));
    in.keys.set(*engine::script::key_from_name("space"));
    start();
    engine::script::ScriptServices services;
    services.input = &in;
    runtime->set_services(services);
    frames();
    const auto log = script_log();
    EXPECT_TRUE(has_line(log, "keys\ttrue\ttrue\ttrue")) << ::testing::PrintToString(log);
    EXPECT_TRUE(has_line(log, "move\tvec3(0, 0, -1)\t0.0"));
    EXPECT_TRUE(has_line(log, "unknown\tfalse"));
    EXPECT_TRUE(has_line(log, "unknown key 'hyperspace'"));

    in.begin_frame(); // space still held: no longer "pressed"
    frames();
    EXPECT_TRUE(has_line(script_log(), "keys\ttrue\tfalse\tfalse"));
}

// Review #111: with several fixed steps in one update, a press reaches
// on_fixed_update once (the runtime's nested latch), for both a held
// down-trigger's edge and a one-frame pressed trigger.
TEST_F(ScriptTest, FixedStepsSeeEachPressOnce) {
    write("count.lua", R"(
        function on_fixed_update(self, dt)
            if input.action_pressed("jump") then self.presses = (self.presses or 0) + 1 end
            if input.action("kick") then self.kicks = (self.kicks or 0) + 1 end
        end
        function on_update(self, dt) print("counts", self.presses or 0, self.kicks or 0) end
    )");
    scripted("c", "count.lua");
    auto map = engine::input::parse_action_map(nlohmann::json::parse(R"({"actions": [
        {"name": "jump", "type": "button", "bindings": ["key:space"]},
        {"name": "kick", "type": "button", "trigger": "pressed", "bindings": ["key:k"]}]})"));
    ASSERT_OK(map);
    engine::input::Actions actions(*map);
    engine::input::DeviceState in;
    in.keys.set(*engine::input::key_from_name("space"));
    in.keys.set(*engine::input::key_from_name("k"));
    start();
    engine::script::ScriptServices services;
    services.input = &in;
    services.actions = &actions;
    runtime->set_services(services);
    actions.update(in, 0.06F);
    runtime->update(0.06F); // three fixed steps of 1/60
    EXPECT_TRUE(has_line(script_log(), "counts\t1\t1"));
}

TEST_F(ScriptTest, CrossScriptCalls) {
    write("counter.lua", R"(
        local M = { properties = { count = 0 } }
        function M.add(self, n) self.count = self.count + n return self.count end
        return M
    )");
    write("caller.lua", R"(
        function on_update(self, dt)
            local other = scene.find("counter"):script()
            print("count", other:add(2))
        end
    )");
    scripted("counter", "counter.lua");
    scripted("caller", "caller.lua");
    start();
    frames(2);
    const auto log = script_log();
    EXPECT_TRUE(has_line(log, "count\t2")) << ::testing::PrintToString(log);
    EXPECT_TRUE(has_line(log, "count\t4"));
}

} // namespace
} // namespace quat::test
