// Binding helpers — see lua_bind.hpp.

#include "lua_bind.hpp"

#include "lua_vm.hpp"

namespace engine::script::detail {

void push_failure(lua_State* L, const Failure& failure) {
    lua_pushlightuserdata(L, const_cast<char*>(&k_failure_sentinel));
    lua_pushlstring(L, failure.message.data(), failure.message.size());
}

int checked_call(lua_State* L) {
    const int nargs = lua_gettop(L);
    lua_pushvalue(L, lua_upvalueindex(1));
    lua_insert(L, 1);
    native_enter(L);
    lua_call(L, nargs, LUA_MULTRET);
    if (!native_leave(L)) { // its results grew the heap past the budget
        lua_pushstring(L, abort_message(L));
        return lua_error(L);
    }
    const int nresults = lua_gettop(L);
    if (nresults >= 2 && lua_touserdata(L, 1) == &k_failure_sentinel) {
        luaL_where(L, 1); // the calling script line
        lua_pushvalue(L, 2);
        lua_concat(L, 2);
        return lua_error(L);
    }
    return nresults;
}

} // namespace engine::script::detail
