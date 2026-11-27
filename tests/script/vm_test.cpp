// G7 review: white-box checks of the sandboxed Lua state (src/script/lua_vm)
// — the engine-binding depth that switches the memory caps off must come
// back to 0 however a binding ends.

#include <gtest/gtest.h>

#include "engine/scene/scene.hpp"
#include "script/runtime_impl.hpp"
#include "support/expect_ok.hpp"
#include "support/temp_dir.hpp"

namespace quat::test {
namespace {

namespace detail = engine::script::detail;

TEST(ScriptVm, BindingDepthRecoversWhenABindingRaisesInACoroutine) {
    TempDir dir("script-vm");
    engine::scene::Scene scene;
    detail::ApiContext ctx; // outlives the state its bindings point at
    ctx.scene = &scene;
    auto vm = detail::LuaVm::create(dir.path(), {});
    ASSERT_OK(vm);
    detail::register_api(**vm, ctx);
    {
        // scene.destroy("x") fails sol2's argument check, which raises from
        // inside the binding; coroutine.resume catches it without any of the
        // engine's own protected calls.
        auto module = (*vm)->load_module("t.lua", R"(
            return { f = function()
                local ok, err = coroutine.resume(coroutine.create(function() scene.destroy("x") end))
                assert(not ok and err ~= nil)
                local wrapped = coroutine.wrap(function() return scene.find(42) end)
                assert(not pcall(wrapped))
            end }
        )");
        ASSERT_TRUE(module.has_value()) << module.error();
        const sol::object fn = module->raw_get<sol::object>("f");
        const auto ok = (*vm)->call(fn);
        EXPECT_TRUE(ok.has_value()) << ok.error();
        EXPECT_EQ((*vm)->native_depth(), 0);
    }
}

} // namespace
} // namespace quat::test
