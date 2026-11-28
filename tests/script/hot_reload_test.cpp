// G7: hot reload — a good edit restarts the script's instances with the new
// code; a broken edit keeps the previous version running and reports the
// error with its line; library edits reach the scripts requiring them; the
// file watcher drives it all.

#include <chrono>
#include <thread>

#include "engine/script/script_source.hpp"
#include "script/script_fixture.hpp"

namespace quat::test {
namespace {

using engine::script::ScriptStatus;

TEST_F(ScriptTest, ReloadRestartsInstancesWithTheNewCode) {
    write("hello.lua", R"(
        function on_start(self) print("v1 start") end
        function on_update(self, dt) self.n = (self.n or 0) + 1 end
        function on_destroy(self) print("v1 destroy", self.n) end
    )");
    scripted("a", "hello.lua");
    scripted("b", "hello.lua");
    start();
    frames(2);
    write("hello.lua", "function on_start(self) print('v2 start', self.n) end");
    runtime->reload("hello.lua");
    frames();
    const auto log = script_log();
    EXPECT_TRUE(has_line(log, "v1 destroy\t2")) << ::testing::PrintToString(log);
    EXPECT_TRUE(has_line(log, "v2 start\tnil")) << "instance state is re-initialised";
    EXPECT_TRUE(has_line(log, "hello.lua: reloaded"));
    const ScriptStatus* s = status("hello.lua");
    ASSERT_NE(s, nullptr);
    EXPECT_EQ(s->state, ScriptStatus::State::ok);
    EXPECT_EQ(s->reloads, 1U);
    EXPECT_EQ(s->instances, 2U);
}

TEST_F(ScriptTest, SyntaxErrorKeepsThePreviousVersion) {
    write("keep.lua", "function on_update(self, dt) print('old code') end");
    scripted("k", "keep.lua");
    start();
    frames();
    ASSERT_TRUE(has_line(script_log(), "old code"));

    write("keep.lua", "function on_update(self, dt)\n  print('new code'\nend");
    runtime->reload("keep.lua");
    frames();
    const auto log = script_log();
    EXPECT_TRUE(has_line(log, "old code")) << ::testing::PrintToString(log);
    EXPECT_FALSE(has_line(log, "new code"));
    EXPECT_TRUE(has_line(log, "previous version keeps running"));
    const ScriptStatus* s = status("keep.lua");
    ASSERT_NE(s, nullptr);
    EXPECT_EQ(s->state, ScriptStatus::State::error);
    EXPECT_NE(s->message.find("keep.lua:3:"), std::string::npos) << s->message;

    // Fixing the file recovers.
    write("keep.lua", "function on_update(self, dt) print('fixed code') end");
    runtime->reload("keep.lua");
    frames();
    EXPECT_TRUE(has_line(script_log(), "fixed code"));
    EXPECT_EQ(status("keep.lua")->state, ScriptStatus::State::ok);
}

TEST_F(ScriptTest, FailedInstancesRestartWhenTheirFileChanges) {
    write("crash.lua", "function on_start(self) error('boom') end");
    scripted("c", "crash.lua");
    start();
    frames();
    EXPECT_EQ(status("crash.lua")->failed, 1U);
    write("crash.lua", "function on_start(self) print('recovered') end");
    runtime->reload("crash.lua");
    frames();
    EXPECT_TRUE(has_line(script_log(), "recovered"));
    EXPECT_EQ(status("crash.lua")->failed, 0U);
    EXPECT_EQ(status("crash.lua")->instances, 1U);
}

TEST_F(ScriptTest, LibraryEditsReloadTheScriptsUsingThem) {
    write("lib/greet.lua", "return { word = 'hello' }");
    write("user.lua", "local g = require('lib.greet') function on_start(self) print('word', g.word) end");
    scripted("u", "user.lua");
    start();
    frames();
    write("lib/greet.lua", "return { word = 'goodbye' }");
    runtime->reload("lib/greet.lua");
    frames();
    const auto log = script_log();
    EXPECT_TRUE(has_line(log, "word\thello")) << ::testing::PrintToString(log);
    EXPECT_TRUE(has_line(log, "word\tgoodbye"));
}

TEST_F(ScriptTest, EditModeReloadChecksSyntax) {
    write("draft.lua", "function on_start(self\nend");
    runtime.emplace(engine::script::ScriptRuntime::Options{dir.path(), {}, false});
    runtime->reload("draft.lua");
    const ScriptStatus* s = status("draft.lua");
    ASSERT_NE(s, nullptr);
    EXPECT_EQ(s->state, ScriptStatus::State::error);
    EXPECT_NE(s->message.find("draft.lua:2:"), std::string::npos) << s->message;
}

TEST_F(ScriptTest, FileWatcherTriggersTheReload) {
    write("watched.lua", "function on_update(self, dt) print('before') end");
    scripted("w", "watched.lua");
    runtime.emplace(engine::script::ScriptRuntime::Options{dir.path(), {}, true});
    ASSERT_OK(runtime->start(scene));
    frames();
    ASSERT_TRUE(has_line(script_log(), "before"));
    std::this_thread::sleep_for(std::chrono::milliseconds(20));
    write("watched.lua", "function on_update(self, dt) print('after') end");
    bool seen = false;
    for (int i = 0; i < 200 && !seen; ++i) { // the polling backend scans every 250 ms
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
        runtime->poll_changes();
        frames();
        seen = has_line(script_log(), "after");
    }
    EXPECT_TRUE(seen);
}

TEST(ScriptSource, SyntaxErrorsCarryLineNumbers) {
    EXPECT_OK(engine::script::check_syntax("local x = 1\nreturn x", "ok.lua"));
    const auto bad = engine::script::check_syntax("local x = 1\nlocal y = = 2\n", "bad.lua");
    ASSERT_FALSE(bad.has_value());
    EXPECT_EQ(bad.error().line, 2);
    EXPECT_EQ(bad.error().message.rfind("bad.lua:2:", 0), 0U) << bad.error().message;
    // Bytecode is never accepted.
    const auto binary = engine::script::check_syntax(std::string("\x1bLua\x54", 5), "bin.lua");
    EXPECT_FALSE(binary.has_value());
}

} // namespace
} // namespace quat::test
