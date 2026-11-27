// G7: per-call instruction and memory limits — a runaway script is stopped
// and reported, cannot catch the limit error, and leaves the others running.

#include "script/script_fixture.hpp"

namespace quat::test {
namespace {

using engine::script::ScriptStatus;

TEST_F(ScriptTest, InfiniteLoopIsStoppedAndReported) {
    write("hang.lua", "function on_update(self, dt)\n  while true do end\nend");
    write("ok.lua", "function on_update(self, dt) self.ticks = (self.ticks or 0) + 1 "
                    "if self.ticks == 3 then print('still running') end end");
    scripted("hang", "hang.lua");
    scripted("ok", "ok.lua");
    engine::script::ScriptLimits limits;
    limits.call_instructions = 200'000;
    start(limits);
    frames(3);
    const auto log = script_log();
    EXPECT_TRUE(has_line(log, "instruction limit exceeded")) << ::testing::PrintToString(log);
    EXPECT_TRUE(has_line(log, "hang.lua:2")) << "the error names the looping line";
    EXPECT_TRUE(has_line(log, "still running")) << "other scripts keep running";
    const ScriptStatus* s = status("hang.lua");
    ASSERT_NE(s, nullptr);
    EXPECT_EQ(s->state, ScriptStatus::State::error);
    EXPECT_EQ(s->failed, 1U);
}

TEST_F(ScriptTest, PcallCannotSwallowTheInstructionLimit) {
    write("sneaky.lua", R"(
        function on_update(self, dt)
            while true do
                pcall(function() while true do end end)
            end
        end
    )");
    scripted("sneaky", "sneaky.lua");
    engine::script::ScriptLimits limits;
    limits.call_instructions = 100'000;
    start(limits);
    frames();
    EXPECT_TRUE(has_line(script_log(), "instruction limit exceeded"));
    EXPECT_EQ(status("sneaky.lua")->failed, 1U);
}

TEST_F(ScriptTest, MemoryBombIsStoppedAndReported) {
    write("bomb.lua", R"(
        function on_start(self)
            local t = {}
            local s = string.rep("x", 1024 * 1024)
            for i = 1, 100000 do t[i] = s .. i end
        end
    )");
    write("rep.lua", "function on_start(self) local s = string.rep('y', 1 << 40) end");
    scripted("bomb", "bomb.lua");
    scripted("rep", "rep.lua");
    engine::script::ScriptLimits limits;
    limits.memory_bytes = 64U << 20;
    limits.call_memory_bytes = 16U << 20;
    start(limits);
    frames();
    const auto log = script_log();
    EXPECT_TRUE(has_line(log, "bomb.lua (entity 'bomb' (id")) << ::testing::PrintToString(log);
    EXPECT_TRUE(has_line(log, "memory limit exceeded")) << ::testing::PrintToString(log);
    EXPECT_EQ(status("bomb.lua")->failed, 1U);
    EXPECT_EQ(status("rep.lua")->failed, 1U);

    // The state is still usable afterwards: a new script runs normally.
    write("after.lua", "function on_start(self) print('after the bomb') end");
    scripted("after", "after.lua");
    frames();
    EXPECT_TRUE(has_line(script_log(), "after the bomb"));
}

TEST_F(ScriptTest, LoadTimeLoopIsStoppedToo) {
    write("toplevel.lua", "while true do end");
    scripted("top", "toplevel.lua");
    engine::script::ScriptLimits limits;
    limits.call_instructions = 100'000;
    start(limits);
    frames();
    const ScriptStatus* s = status("toplevel.lua");
    ASSERT_NE(s, nullptr);
    EXPECT_EQ(s->state, ScriptStatus::State::error);
    EXPECT_NE(s->message.find("instruction limit"), std::string::npos) << s->message;
}

// Review G7 #1: the error handler runs without limits, so it must not call a
// script's __tostring (this one never returns).
TEST_F(ScriptTest, ErrorObjectsWithLoopingTostringCannotHang) {
    write("evil_error.lua", R"(
        function on_start(self)
            error(setmetatable({}, { __tostring = function() while true do end end }))
        end
    )");
    scripted("evil", "evil_error.lua");
    engine::script::ScriptLimits limits;
    limits.call_instructions = 100'000;
    start(limits);
    frames(); // returns: the handler never ran the metamethod
    const ScriptStatus* s = status("evil_error.lua");
    ASSERT_NE(s, nullptr);
    EXPECT_EQ(s->failed, 1U);
    EXPECT_NE(s->message.find("error object is a table value"), std::string::npos) << s->message;
}

// Review G7 #2: finalizers would run outside any budget (between callbacks,
// and in lua_close at Stop / quit), so scripts cannot install __gc.
TEST_F(ScriptTest, GcMetamethodsAreRefusedAndStopCompletes) {
    write("finalizer.lua", R"(
        function on_start(self)
            print("gc", pcall(setmetatable, {}, { __gc = function() while true do end end }))
            local mt = {}
            local t = setmetatable({}, mt)
            mt.__gc = function() while true do end end -- added later: never a finalizer
            print("plain", getmetatable(t) == mt)
            t = nil
        end
    )");
    scripted("f", "finalizer.lua");
    start();
    frames();
    const auto log = script_log();
    EXPECT_TRUE(has_line(log, "gc\tfalse")) << ::testing::PrintToString(log);
    EXPECT_TRUE(has_line(log, "__gc finalizers are not available"));
    EXPECT_TRUE(has_line(log, "plain\ttrue"));
    runtime->stop(); // lua_close runs no script finalizer: this returns
    EXPECT_FALSE(runtime->running());
}

// Review G7 #5: an engine call's results may exceed the per-call budget only
// until it returns; then the script is stopped.
TEST_F(ScriptTest, EngineCallResultsCountAgainstTheBudget) {
    write("hoarder.lua", R"(
        function on_start(self)
            local keep = {}
            for i = 1, 100000 do keep[i] = scene.find_all("x") end
        end
    )");
    for (int i = 0; i < 4; ++i) scene.create_entity("x");
    scripted("h", "hoarder.lua");
    engine::script::ScriptLimits limits;
    limits.call_memory_bytes = 4U << 20;
    start(limits);
    frames();
    const ScriptStatus* s = status("hoarder.lua");
    ASSERT_NE(s, nullptr);
    EXPECT_EQ(s->failed, 1U);
    EXPECT_NE(s->message.find("memory limit exceeded"), std::string::npos) << s->message;
}

} // namespace
} // namespace quat::test
