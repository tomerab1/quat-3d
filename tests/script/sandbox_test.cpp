// G7: the script sandbox — dangerous libraries are absent, require() stays
// inside assets/scripts/, engine tables are read-only, and a script cannot
// reach the engine's metatables.

#include "script/script_fixture.hpp"

namespace quat::test {
namespace {

TEST_F(ScriptTest, DangerousLibrariesAreAbsent) {
    write("probe.lua", R"(
        function on_start(self)
            print("io", io, "os", os, "package", package, "debug", debug)
            print("load", load, "loadfile", loadfile, "dofile", dofile,
                  "collectgarbage", collectgarbage)
            print("string.dump", string.dump ~= nil)
        end
    )");
    scripted("probe", "probe.lua");
    start();
    frames();
    const auto log = script_log();
    EXPECT_TRUE(has_line(log, "io\tnil\tos\tnil\tpackage\tnil\tdebug\tnil")) << ::testing::PrintToString(log);
    EXPECT_TRUE(has_line(log, "load\tnil\tloadfile\tnil\tdofile\tnil\tcollectgarbage\tnil"));
    // string.dump only serializes functions; it cannot load bytecode back
    // (load is gone and chunks are compiled as text only).
    EXPECT_TRUE(has_line(log, "string.dump"));
}

TEST_F(ScriptTest, RequireResolvesOnlyUnderTheScriptsDirectory) {
    write("lib/util.lua", "local M = {} function M.twice(x) return 2 * x end return M");
    write("main.lua", R"(
        local util = require("lib.util")
        function on_start(self)
            print("twice", util.twice(21))
            print("same", require("lib/util") == util)
            local ok, err = pcall(require, "../../etc/passwd")
            print("escape", ok, err)
            ok, err = pcall(require, "/etc/hosts")
            print("absolute", ok, err)
            ok, err = pcall(require, "missing")
            print("missing", ok)
        end
    )");
    scripted("main", "main.lua");
    start();
    frames();
    const auto log = script_log();
    EXPECT_TRUE(has_line(log, "twice\t42")) << ::testing::PrintToString(log);
    EXPECT_TRUE(has_line(log, "same\ttrue"));
    EXPECT_TRUE(has_line(log, "escape\tfalse")) << ::testing::PrintToString(log);
    EXPECT_TRUE(has_line(log, "absolute\tfalse"));
    EXPECT_TRUE(has_line(log, "missing\tfalse"));
}

TEST_F(ScriptTest, EngineTablesAndMetatablesAreProtected) {
    write("evil.lua", R"(
        function on_start(self)
            print("math.write", pcall(function() math.floor = nil end))
            print("scene.write", pcall(function() scene.find = nil end))
            print("string.mt", getmetatable("x"))
            print("vec3.mt", getmetatable(vec3(1, 2, 3)))
            print("env.mt", getmetatable(_ENV))
            print("self.mt", pcall(setmetatable, self, {}))
            print("floor", math.floor(2.5))
        end
    )");
    scripted("evil", "evil.lua");
    start();
    frames();
    const auto log = script_log();
    EXPECT_TRUE(has_line(log, "math.write\tfalse")) << ::testing::PrintToString(log);
    EXPECT_TRUE(has_line(log, "scene.write\tfalse"));
    EXPECT_TRUE(has_line(log, "string.mt\tnil"));
    EXPECT_TRUE(has_line(log, "vec3.mt\tnil"));
    EXPECT_TRUE(has_line(log, "env.mt\tfalse"));
    EXPECT_TRUE(has_line(log, "self.mt\tfalse"));
    EXPECT_TRUE(has_line(log, "floor\t2"));
}

TEST_F(ScriptTest, ScriptsHaveSeparateGlobals) {
    write("a.lua", "counter = 1 function on_start(self) print('a', counter) end");
    write("b.lua", "function on_start(self) print('b', counter) end");
    scripted("a", "a.lua");
    scripted("b", "b.lua");
    start();
    frames();
    const auto log = script_log();
    EXPECT_TRUE(has_line(log, "a.lua: a\t1")) << ::testing::PrintToString(log);
    EXPECT_TRUE(has_line(log, "b.lua: b\tnil"));
}

// Review G7 #3: rawset bypasses __newindex, and the library / engine tables
// are shared by every script.
TEST_F(ScriptTest, RawsetCannotReplaceSharedTables) {
    write("vandal.lua", R"(
        function on_start(self)
            print("math", pcall(rawset, math, "floor", function() return 42 end))
            print("scene", pcall(rawset, scene, "find", nil))
            local own = {}
            rawset(own, "k", 1)
            print("own", own.k)
        end
    )");
    write("victim.lua", "function on_update(self, dt) print('floor', math.floor(2.5), scene.find ~= nil) end");
    scripted("vandal", "vandal.lua");
    scripted("victim", "victim.lua");
    start();
    frames();
    const auto log = script_log();
    EXPECT_TRUE(has_line(log, "math\tfalse")) << ::testing::PrintToString(log);
    EXPECT_TRUE(has_line(log, "scene\tfalse"));
    EXPECT_TRUE(has_line(log, "read-only"));
    EXPECT_TRUE(has_line(log, "own\t1"));
    EXPECT_TRUE(has_line(log, "floor\t2\ttrue")) << "the other script sees the originals";
}

// Review G7 #6: extra arguments to require must not shift its stack slots.
TEST_F(ScriptTest, RequireWithExtraArgumentsIsCached) {
    write("lib/counter.lua", "count = (count or 0) + 1 return { loads = count }");
    write("user.lua", R"(
        function on_start(self)
            local a = require("lib.counter", "extra", 42)
            local b = require("lib.counter", {})
            print("cached", a == b, a.loads)
        end
    )");
    scripted("u", "user.lua");
    start();
    frames();
    EXPECT_TRUE(has_line(script_log(), "cached\ttrue\t1"));
}

} // namespace
} // namespace quat::test
