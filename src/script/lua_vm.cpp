// Sandboxed Lua state with limits — see lua_vm.hpp.
//
// The C callbacks below (allocator, hook, pcall wrappers, require) may raise
// Lua errors, which unwind with longjmp: they hold no C++ objects with
// destructors at the point where they raise.

#include "lua_vm.hpp"

#include <cstdio>
#include <cstdlib>

#include "engine/core/log.hpp"
#include "engine/scene/script_component.hpp"
#include "engine/script/script_source.hpp"

namespace engine::script::detail {

namespace {

constexpr int k_hook_interval = 1000; // instructions between budget checks
constexpr int k_max_traceback_lines = 10;
constexpr char k_loaded_key[] = "quat.loaded"; // registry: path -> module

[[nodiscard]] VmState* vm_of(lua_State* L) {
    void* ud = nullptr;
    (void)lua_getallocf(L, &ud);
    return static_cast<VmState*>(ud);
}

void* vm_alloc(void* ud, void* ptr, std::size_t osize, std::size_t nsize) {
    auto* vm = static_cast<VmState*>(ud);
    const std::size_t old = ptr != nullptr ? osize : 0;
    if (nsize == 0) {
        std::free(ptr);
        vm->bytes -= old;
        return nullptr;
    }
    if (nsize > old && vm->call_depth > 0 && !vm->unlimited) {
        const std::size_t after = vm->bytes - old + nsize;
        const std::size_t call_cap = vm->call_start_bytes + vm->limits.call_memory_bytes;
        if (after > vm->limits.memory_bytes || after > call_cap) {
            if (!vm->aborting) {
                vm->aborting = true;
                std::snprintf(vm->abort_message, sizeof(vm->abort_message),
                              "memory limit exceeded (%zu MiB per script state, %zu MiB per call)",
                              vm->limits.memory_bytes >> 20, vm->limits.call_memory_bytes >> 20);
            }
            return nullptr; // Lua collects garbage and retries, then raises LUA_ERRMEM
        }
    }
    void* p = std::realloc(ptr, nsize);
    if (p == nullptr) return nullptr;
    vm->bytes = vm->bytes - old + nsize;
    return p;
}

void count_hook(lua_State* L, lua_Debug* /*ar*/) {
    VmState* vm = vm_of(L);
    if (vm->call_depth == 0 || vm->unlimited) return;
    vm->instructions += k_hook_interval;
    if (vm->instructions <= vm->limits.call_instructions && !vm->aborting) return;
    if (!vm->aborting) {
        vm->aborting = true;
        std::snprintf(vm->abort_message, sizeof(vm->abort_message),
                      "instruction limit exceeded (%llu per call): an endless loop?",
                      static_cast<unsigned long long>(vm->limits.call_instructions));
    }
    luaL_error(L, "%s", vm->abort_message);
}

int vm_panic(lua_State* L) {
    const char* msg = lua_tostring(L, -1);
    std::fprintf(stderr, "[script][error] unprotected Lua error: %s\n",
                 msg != nullptr ? msg : "(not a string)");
    std::abort();
}

// Message handler for protected calls: the error plus a short traceback.
int message_handler(lua_State* L) {
    VmState* vm = vm_of(L);
    vm->unlimited = true;
    const char* msg = lua_tostring(L, 1);
    if (msg == nullptr) {
        if (luaL_callmeta(L, 1, "__tostring") != 0 && lua_type(L, -1) == LUA_TSTRING) {
            msg = lua_tostring(L, -1);
        } else {
            msg = lua_pushfstring(L, "(error object is a %s value)", luaL_typename(L, 1));
        }
    }
    luaL_traceback(L, L, msg, 1);
    vm->unlimited = false;
    return 1;
}

// pcall / xpcall for scripts: a limit error is re-raised instead of caught.
int protected_k(lua_State* L, int /*status*/, lua_KContext /*ctx*/) {
    const VmState* vm = vm_of(L);
    if (vm->aborting) return luaL_error(L, "%s", vm->abort_message);
    return lua_gettop(L);
}

int sandbox_pcall(lua_State* L) {
    lua_pushvalue(L, lua_upvalueindex(1)); // the real pcall / xpcall
    lua_insert(L, 1);
    lua_callk(L, lua_gettop(L) - 1, LUA_MULTRET, 0, protected_k);
    return protected_k(L, LUA_OK, 0);
}

// getmetatable for scripts: plain tables only.
int sandbox_getmetatable(lua_State* L) {
    luaL_checkany(L, 1);
    if (lua_type(L, 1) != LUA_TTABLE || lua_getmetatable(L, 1) == 0) {
        lua_pushnil(L);
        return 1;
    }
    if (luaL_getmetafield(L, 1, "__metatable") != LUA_TNIL) return 1;
    return 1; // the metatable pushed by lua_getmetatable
}

int readonly_newindex(lua_State* L) {
    return luaL_error(L, "attempt to modify a read-only engine table (field '%s')",
                      luaL_tolstring(L, 2, nullptr));
}

// Calling a proxy forwards to the proxied value (vec3(...), quat(...)).
int proxy_call(lua_State* L) {
    lua_pushvalue(L, lua_upvalueindex(1));
    lua_replace(L, 1); // the proxy itself is the first argument: swap in the target
    lua_call(L, lua_gettop(L) - 1, LUA_MULTRET);
    return lua_gettop(L);
}

// Pushes a read-only proxy of the value at `index`.
void push_readonly(lua_State* L, int index) {
    index = lua_absindex(L, index);
    lua_newtable(L);                 // proxy
    lua_createtable(L, 0, 4);        // its metatable
    lua_pushvalue(L, index);
    lua_setfield(L, -2, "__index");
    lua_pushcfunction(L, readonly_newindex);
    lua_setfield(L, -2, "__newindex");
    lua_pushboolean(L, 0);
    lua_setfield(L, -2, "__metatable");
    lua_pushvalue(L, index);
    lua_pushcclosure(L, proxy_call, 1);
    lua_setfield(L, -2, "__call");
    lua_setmetatable(L, -2);
}

// Creates a script environment for the function on top of the stack: a new
// table whose missing globals come from the sandbox (upvalue / argument).
void set_environment(lua_State* L, int fn_index, int sandbox_index) {
    fn_index = lua_absindex(L, fn_index);
    sandbox_index = lua_absindex(L, sandbox_index);
    lua_newtable(L);           // env
    lua_createtable(L, 0, 2);  // env metatable
    lua_pushvalue(L, sandbox_index);
    lua_setfield(L, -2, "__index");
    lua_pushboolean(L, 0);
    lua_setfield(L, -2, "__metatable");
    lua_setmetatable(L, -2);
    if (lua_setupvalue(L, fn_index, 1) == nullptr) lua_pop(L, 1); // _ENV
}

// require(name): "lib.util" or "lib/util" -> assets/scripts/lib/util.lua,
// run once per state in its own environment; the result is cached.
int sandbox_require(lua_State* L) {
    const char* name = luaL_checkstring(L, 1);
    char error[512] = {};
    char chunk[256] = {};
    bool compiled = false;
    {
        std::string rel(name);
        if (!rel.ends_with(".lua")) {
            for (char& c : rel) c = c == '.' ? '/' : c;
            rel += ".lua";
        }
        std::snprintf(chunk, sizeof(chunk), "%s", rel.c_str());
        auto file = script_file(vm_of(L)->scripts_root, rel);
        if (!file) {
            std::snprintf(error, sizeof(error), "require '%s': %s", name,
                          file.error().message.c_str());
        } else if (lua_getfield(L, LUA_REGISTRYINDEX, k_loaded_key) == LUA_TTABLE &&
                   lua_getfield(L, -1, rel.c_str()) != LUA_TNIL) {
            if (lua_type(L, -1) == LUA_TBOOLEAN && lua_toboolean(L, -1) == 0) {
                std::snprintf(error, sizeof(error), "require '%s': circular require", name);
            } else {
                return 1; // cached module (no C++ object left to destroy here)
            }
        } else if (auto source = read_script(*file); !source) {
            std::snprintf(error, sizeof(error), "require '%s': %s", name,
                          source.error().message.c_str());
        } else {
            const std::string chunk_name = "@" + rel;
            if (luaL_loadbufferx(L, source->data(), source->size(), chunk_name.c_str(), "t") !=
                LUA_OK) {
                std::snprintf(error, sizeof(error), "%s", lua_tostring(L, -1));
                lua_pop(L, 1);
            } else {
                compiled = true;
            }
        }
    }
    if (!compiled) return luaL_error(L, "%s", error);
    // Stack: name, loaded, (nil), fn
    const int fn = lua_gettop(L);
    lua_getfield(L, LUA_REGISTRYINDEX, "quat.sandbox");
    set_environment(L, fn, -1);
    lua_pop(L, 1);
    lua_pushboolean(L, 0); // loading marker (circular require detection)
    lua_setfield(L, 2, chunk);
    lua_pushvalue(L, fn);
    lua_call(L, 0, 1);
    if (lua_isnil(L, -1)) {
        lua_pop(L, 1);
        lua_pushboolean(L, 1);
    }
    lua_pushvalue(L, -1);
    lua_setfield(L, 2, chunk);
    return 1;
}

} // namespace

std::expected<std::unique_ptr<LuaVm>, core::Error> LuaVm::create(const std::filesystem::path& scripts_root,
                                                                 const VmLimits& limits) {
    auto vm = std::make_unique<LuaVm>(Token{});
    vm->vm_ = std::make_unique<VmState>();
    vm->vm_->limits = limits;
    vm->vm_->scripts_root = scripts_root;
    vm->lua_ = std::make_unique<sol::state>(&vm_panic, &vm_alloc, vm->vm_.get());
    if (vm->lua_->lua_state() == nullptr) {
        return std::unexpected(core::Error{"cannot create a Lua state"});
    }
    lua_sethook(vm->state(), &count_hook, LUA_MASKCOUNT, k_hook_interval);
    vm->build_sandbox();
    return vm;
}

LuaVm::~LuaVm() {
    sandbox_ = sol::table(); // release the reference before the state closes
}

void LuaVm::build_sandbox() {
    sol::state& lua = *lua_;
    lua.open_libraries(sol::lib::base, sol::lib::string, sol::lib::table, sol::lib::math,
                       sol::lib::utf8, sol::lib::coroutine);
    lua_State* L = state();
    lua_newtable(L);
    const int box = lua_gettop(L);
    static constexpr const char* k_safe_base[] = {
        "assert", "error",  "ipairs", "next",     "pairs",  "select",       "tonumber",
        "tostring", "type", "rawequal", "rawget", "rawset", "rawlen",       "setmetatable",
        "_VERSION"};
    for (const char* name : k_safe_base) {
        lua_getglobal(L, name);
        lua_setfield(L, box, name);
    }
    for (const char* name : {"pcall", "xpcall"}) {
        lua_getglobal(L, name);
        lua_pushcclosure(L, &sandbox_pcall, 1);
        lua_setfield(L, box, name);
    }
    lua_pushcfunction(L, &sandbox_getmetatable);
    lua_setfield(L, box, "getmetatable");
    lua_pushcfunction(L, &sandbox_require);
    lua_setfield(L, box, "require");
    for (const char* lib : {"string", "table", "math", "utf8", "coroutine"}) {
        lua_getglobal(L, lib);
        push_readonly(L, -1);
        lua_setfield(L, box, lib);
        lua_pop(L, 1);
    }
    lua_pushvalue(L, box);
    lua_setfield(L, LUA_REGISTRYINDEX, "quat.sandbox");
    lua_newtable(L);
    lua_setfield(L, LUA_REGISTRYINDEX, k_loaded_key);
    sandbox_ = sol::table(L, box);
    lua_settop(L, box - 1);
}

void LuaVm::expose(std::string_view name, const sol::object& value) {
    lua_State* L = state();
    sandbox_.push(L);
    value.push(L);
    if (lua_type(L, -1) == LUA_TTABLE) {
        push_readonly(L, -1);
        lua_remove(L, -2);
    }
    lua_setfield(L, -2, std::string(name).c_str());
    lua_pop(L, 1);
}

std::expected<sol::table, std::string> LuaVm::load_module(std::string_view path,
                                                          std::string_view source) {
    lua_State* L = state();
    const int base = lua_gettop(L);
    const std::string chunk_name = "@" + std::string(path);
    if (luaL_loadbufferx(L, source.data(), source.size(), chunk_name.c_str(), "t") != LUA_OK) {
        std::string msg = lua_tostring(L, -1) != nullptr ? lua_tostring(L, -1) : "compile error";
        lua_settop(L, base);
        return std::unexpected(std::move(msg));
    }
    sandbox_.push(L);
    set_environment(L, base + 1, -1);
    lua_pop(L, 1);
    lua_getupvalue(L, base + 1, 1); // keep the environment (base + 2)
    lua_insert(L, base + 1);        // env, fn
    if (auto ok = finish_call(base + 1, 0, 1); !ok) {
        lua_settop(L, base);
        return std::unexpected(ok.error());
    }
    // Stack: env, result
    const int type = lua_type(L, -1);
    if (type == LUA_TNIL) {
        lua_pop(L, 1); // the environment is the module
    } else if (type != LUA_TTABLE) {
        lua_settop(L, base);
        return std::unexpected(std::string(path) +
                               ": a script returns a table of callbacks (or nothing, defining "
                               "global functions), not a " + lua_typename(L, type));
    }
    sol::table module(L, -1);
    lua_settop(L, base);
    return module;
}

std::expected<void, std::string> LuaVm::finish_call(int base, int nargs, int nresults) {
    lua_State* L = state();
    VmState& vm = *vm_;
    lua_pushcfunction(L, &message_handler);
    lua_insert(L, base + 1); // below the function
    if (vm.call_depth++ == 0) {
        vm.instructions = 0;
        vm.aborting = false;
        vm.call_start_bytes = vm.bytes;
    }
    const int status = lua_pcall(L, nargs, nresults, base + 1);
    --vm.call_depth;
    if (status == LUA_OK) {
        lua_remove(L, base + 1);
        return {};
    }
    std::string msg;
    if (status == LUA_ERRMEM) { // no message handler runs for memory errors
        msg = vm.aborting ? vm.abort_message : "out of memory";
    } else {
        msg = lua_tostring(L, -1) != nullptr ? lua_tostring(L, -1) : "unknown error";
    }
    lua_settop(L, base);
    if (vm.call_depth == 0) vm.aborting = false;
    // Keep the message and the first traceback lines.
    std::size_t pos = 0;
    for (int line = 0; line <= k_max_traceback_lines && pos != std::string::npos; ++line) {
        pos = msg.find('\n', pos + 1);
    }
    if (pos != std::string::npos) msg.resize(pos);
    return std::unexpected(std::move(msg));
}

} // namespace engine::script::detail
