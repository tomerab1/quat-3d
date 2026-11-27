// G7: script lifecycle — callback order, properties and overrides,
// deferred destroys with on_destroy, disabling and session reset.

#include "script/script_fixture.hpp"

namespace quat::test {
namespace {

using engine::scene::Script;
using engine::scene::ScriptProperty;
using engine::scene::ScriptValueType;

TEST_F(ScriptTest, CallbacksRunInOrder) {
    write("order.lua", R"(
        local M = {}
        function M.on_start(self) print("start") end
        function M.on_fixed_update(self, dt) if not self.fixed then self.fixed = true print("fixed", dt) end end
        function M.on_update(self, dt) self.n = (self.n or 0) + 1 print("update", self.n) end
        function M.on_destroy(self) print("destroy", self.entity:valid()) end
        return M
    )");
    const entt::entity e = scripted("thing", "order.lua");
    start();
    frames(2);
    scene.registry().remove<Script>(e);
    frames();
    const auto log = script_log();
    ASSERT_GE(log.size(), 5U) << ::testing::PrintToString(log);
    EXPECT_EQ(log[0], "order.lua: start");
    EXPECT_EQ(log[1].rfind("order.lua: fixed\t0.01666", 0), 0U) << log[1];
    EXPECT_EQ(log[2], "order.lua: update\t1");
    EXPECT_EQ(log[3], "order.lua: update\t2");
    EXPECT_EQ(log[4], "order.lua: destroy\ttrue");
}

TEST_F(ScriptTest, PausedClockHoldsScripts) {
    write("tick.lua", "function on_update(self, dt) print('tick') end");
    scripted("t", "tick.lua");
    start();
    runtime->update(0.0F);
    EXPECT_TRUE(script_log().empty());
    frames();
    EXPECT_TRUE(has_line(script_log(), "tick"));
}

TEST_F(ScriptTest, DeclaredPropertiesAndOverrides) {
    write("props.lua", R"(
        return {
            properties = { speed = 3, label = "coin", spin = true, offset = vec3(0, 1, 0) },
            on_start = function(self)
                print("props", self.speed, self.label, self.spin, tostring(self.offset))
            end,
            on_update = function(self, dt) print("speed", self.speed) end,
        }
    )");
    const entt::entity a = scripted("a", "props.lua");
    const entt::entity b = scripted("b", "props.lua");
    {
        Script& s = scene.registry().get<Script>(b);
        ScriptProperty p;
        p.name = "speed";
        p.type = ScriptValueType::number;
        p.number = 7.5;
        s.properties.push_back(p);
    }
    start();
    frames();
    auto log = script_log();
    EXPECT_TRUE(has_line(log, "props\t3\tcoin\ttrue\tvec3(0, 1, 0)")) << ::testing::PrintToString(log);
    EXPECT_TRUE(has_line(log, "speed\t7.5"));

    // A property edit while playing reaches the running instance.
    Script& s = scene.registry().get<Script>(a);
    ScriptProperty p;
    p.name = "speed";
    p.number = 11;
    s.properties.push_back(p);
    ++s.revision;
    frames();
    EXPECT_TRUE(has_line(script_log(), "speed\t11"));

    // The inspector's view of the declared properties (edit mode, scratch state).
    runtime->stop();
    auto declared = runtime->declared_properties("props.lua");
    ASSERT_OK(declared);
    ASSERT_EQ(declared->size(), 4U);
    EXPECT_EQ((*declared)[0].name, "label");
    EXPECT_EQ((*declared)[0].type, ScriptValueType::string);
    EXPECT_EQ((*declared)[1].name, "offset");
    EXPECT_EQ((*declared)[1].type, ScriptValueType::vec3);
    EXPECT_FLOAT_EQ((*declared)[1].vec.y, 1.0F);
    EXPECT_EQ((*declared)[2].name, "speed");
    EXPECT_DOUBLE_EQ((*declared)[2].number, 3.0);
    EXPECT_EQ((*declared)[3].type, ScriptValueType::boolean);
}

TEST_F(ScriptTest, DestroyIsDeferredAndRunsOnDestroyForTheSubtree) {
    write("killer.lua", R"(
        function on_update(self, dt)
            local victim = scene.find("victim")
            if victim then
                victim:destroy()
                print("still valid this frame", victim:valid())
            end
        end
    )");
    write("victim.lua", "function on_destroy(self) print('victim gone') end");
    write("child.lua", "function on_destroy(self) print('child gone') end");
    scripted("killer", "killer.lua");
    const entt::entity victim = scripted("victim", "victim.lua");
    const entt::entity child = scripted("child", "child.lua");
    scene.set_parent(child, victim);
    start();
    frames();
    const auto log = script_log();
    EXPECT_TRUE(has_line(log, "still valid this frame\ttrue")) << ::testing::PrintToString(log);
    EXPECT_TRUE(has_line(log, "victim gone"));
    EXPECT_TRUE(has_line(log, "child gone"));
    EXPECT_FALSE(scene.registry().valid(victim));
    EXPECT_FALSE(scene.registry().valid(child));
}

TEST_F(ScriptTest, DisablingEndsTheInstanceAndStopResetsState) {
    write("count.lua", R"(
        total = total or 0
        function on_start(self) total = total + 1 print("starts", total) end
        function on_destroy(self) print("ended") end
    )");
    const entt::entity e = scripted("c", "count.lua");
    start();
    frames();
    scene.registry().get<Script>(e).enabled = false;
    frames();
    scene.registry().get<Script>(e).enabled = true;
    frames();
    auto log = script_log();
    EXPECT_TRUE(has_line(log, "ended"));
    EXPECT_TRUE(has_line(log, "starts\t1"));
    EXPECT_TRUE(has_line(log, "starts\t2")) << "the module ran once; its globals persist";

    // A new session starts from a fresh state.
    runtime->stop();
    ASSERT_OK(runtime->start(scene));
    frames();
    log = script_log();
    EXPECT_TRUE(has_line(log, "starts\t1")) << ::testing::PrintToString(log);
}

TEST_F(ScriptTest, ErrorsCarryTheScriptLine) {
    write("broken.lua", "function on_start(self)\n  local x = nil\n  print(x.field)\nend");
    scripted("broken", "broken.lua");
    start();
    frames();
    const auto log = script_log();
    EXPECT_TRUE(has_line(log, "broken.lua:3: attempt to index a nil value")) << ::testing::PrintToString(log);
    EXPECT_EQ(status("broken.lua")->state, engine::script::ScriptStatus::State::error);
}

TEST_F(ScriptTest, EngineCallErrorsPointAtTheCallingLine) {
    write("stale.lua", R"(
        function on_start(self)
            local e = scene.spawn({ name = "temp" })
            scene.destroy(e)
        end
        function on_update(self, dt)
            local e = scene.find("temp")
            if e == nil and not self.done then
                self.done = true
                local ghost = scene.spawn({ name = "ghost" })
                ghost:destroy()
                self.ghost = ghost
            elseif self.ghost then
                self.ghost:position()
            end
        end
    )");
    scripted("stale", "stale.lua");
    start();
    frames(3);
    const auto log = script_log();
    EXPECT_TRUE(has_line(log, "stale.lua:14: entity")) << ::testing::PrintToString(log);
    EXPECT_TRUE(has_line(log, "no longer exists"));
}

} // namespace
} // namespace quat::test
