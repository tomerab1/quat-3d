#pragma once

// LuaVm — one sandboxed Lua 5.4 state with resource limits (G7). Private to
// engine_script.
//
// Sandbox: scripts see only the safe parts of the base library (no load /
// loadfile / dofile / collectgarbage), the string, table, math, utf8 and
// coroutine libraries as read-only tables, a `require` that resolves module
// names to files under assets/scripts/, and whatever the engine API exposes.
// io, os, package and debug are never opened. Each script file runs in its
// own environment table (its globals), whose lookups fall back to the shared
// sandbox; getmetatable() hides the metatables of everything but plain
// tables, so scripts cannot reach the engine's bindings or each other's
// environments.
//
// Limits: every protected call (a script callback, a module's first run)
// gets an instruction budget (a count hook) and a memory budget (the
// allocator: a cap for the whole state plus the growth one call may cause).
// Exceeding either raises a Lua error that the script's own pcall/xpcall
// cannot swallow, so the callback ends and the runtime disables the
// instance.

#include <cstddef>
#include <cstdint>
#include <expected>
#include <filesystem>
#include <memory>
#include <string>
#include <string_view>
#include <utility>

#include "engine/core/error.hpp"
#include "sol_config.hpp"

namespace engine::script::detail {

struct VmLimits {
    std::size_t   memory_bytes = 256U << 20;     // the whole state
    std::size_t   call_memory_bytes = 64U << 20; // growth within one call
    std::uint64_t call_instructions = 5'000'000; // VM instructions per call
};

// Shared with the C callbacks (the allocator's user data).
struct VmState {
    VmLimits              limits;
    std::filesystem::path scripts_root;
    std::size_t           bytes = 0;
    std::size_t           call_start_bytes = 0;
    std::uint64_t         instructions = 0;
    int                   call_depth = 0;   // > 0 inside a protected call
    // > 0 while an engine binding (C++) runs: its allocations are not capped,
    // because a failed allocation would longjmp over its live C++ objects;
    // the budget is checked when it returns (native_leave).
    int                   in_native = 0;
    bool                  unlimited = false; // the error handler (C only) allocates freely
    bool                  aborting = false; // a limit was hit in the current call
    char                  abort_message[160] = {};
};

// Brackets an engine binding called from a script (checked_call).
void native_enter(lua_State* L);
// False when the call's heap went over budget meanwhile; the abort is armed
// and abort_message() says why.
[[nodiscard]] bool native_leave(lua_State* L);
[[nodiscard]] const char* abort_message(lua_State* L);

class LuaVm {
    struct Token {}; // create() is the only way to make one

public:
    [[nodiscard]] static std::expected<std::unique_ptr<LuaVm>, core::Error>
    create(const std::filesystem::path& scripts_root, const VmLimits& limits);

    explicit LuaVm(Token /*token*/) {}

    LuaVm(const LuaVm&) = delete;
    LuaVm& operator=(const LuaVm&) = delete;
    LuaVm(LuaVm&&) = delete;
    LuaVm& operator=(LuaVm&&) = delete;
    ~LuaVm();

    [[nodiscard]] lua_State* state() const { return lua_->lua_state(); }
    [[nodiscard]] sol::state_view lua() const { return sol::state_view(state()); }

    // Adds a global every script sees. Tables (and sol2 usertype tables) are
    // exposed through read-only proxies.
    void expose(std::string_view name, const sol::object& value);

    // Compiles `source` as script `path` and runs it once in a fresh
    // environment, under the call limits. The module is what the chunk
    // returns (a table), or its environment when it returns nothing (scripts
    // written as global functions).
    [[nodiscard]] std::expected<sol::table, std::string> load_module(std::string_view path,
                                                                     std::string_view source);

    // Calls `fn(args...)` protected and under the limits (results dropped).
    // Nested calls (a script callback triggering another) share the
    // outermost call's budget. The error is Lua's message plus a short
    // traceback.
    template <typename... Args>
    [[nodiscard]] std::expected<void, std::string> call(const sol::reference& fn, Args&&... args) {
        lua_State* L = state();
        const int base = lua_gettop(L);
        fn.push(L);
        (sol::stack::push(L, std::forward<Args>(args)), ...);
        return finish_call(base, static_cast<int>(sizeof...(Args)), 0);
    }

    // Lua heap in use (bytes).
    [[nodiscard]] std::size_t memory() const { return vm_->bytes; }
    [[nodiscard]] const VmLimits& limits() const { return vm_->limits; }

private:
    void build_sandbox();
    // Runs the function + `nargs` arguments above `base` (results left on the
    // stack above `base` on success; the stack is reset to `base` on error).
    [[nodiscard]] std::expected<void, std::string> finish_call(int base, int nargs, int nresults);

    std::unique_ptr<VmState>    vm_; // outlives the state (its allocator)
    std::unique_ptr<sol::state> lua_;
    sol::table                  sandbox_; // the scripts' shared globals
};

} // namespace engine::script::detail
