// Native Lua modules of game modules (ROADMAP G17.1): require("<name>")
// returns the module's functions; arguments and results cross as JSON, and
// a failure is a Lua error with the message.

#include <gtest/gtest.h>
#include <nlohmann/json.hpp>

#include "script_fixture.hpp"

namespace {

using quat::test::ScriptTest;
using nlohmann::json;

class NativeModule : public ScriptTest {
protected:
    engine::script::ScriptRuntime& start_with_module() {
        engine::script::ScriptRuntime::Options options{dir.path(), {}, false};
        options.modules.push_back(
            {.name = "town",
             .functions = {{.name = "sum",
                            .fn = [](const json& args) -> std::expected<json, engine::core::Error> {
                                double total = 0.0;
                                for (const json& a : args) {
                                    if (!a.is_number()) return std::unexpected(engine::core::Error{"numbers only"});
                                    total += a.get<double>();
                                }
                                return json{{"total", total}, {"count", args.size()}};
                            }}}});
        runtime.emplace(std::move(options));
        EXPECT_OK(runtime->start(scene));
        return *runtime;
    }
};

TEST_F(NativeModule, RequireReturnsTheModule) {
    write("m.lua", R"(
local town = require("town")
local M = {}
function M:on_start()
    local r = town.sum(1, 2, 3.5)
    print("total=" .. r.total .. " count=" .. r.count)
    local ok, err = pcall(town.sum, "x")
    print("failed=" .. tostring(not ok) .. " " .. tostring(err))
end
return M
)");
    start_with_module();
    scripted("E", "m.lua");
    frames(1);
    const auto lines = script_log();
    EXPECT_TRUE(has_line(lines, "total=6.5 count=3")) << (lines.empty() ? "" : lines.back());
    EXPECT_TRUE(has_line(lines, "failed=true")) << (lines.empty() ? "" : lines.back());
    EXPECT_TRUE(has_line(lines, "town.sum: numbers only")) << (lines.empty() ? "" : lines.back());
}

} // namespace
