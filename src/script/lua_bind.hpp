#pragma once

// Binding helpers for the script API (G7). Private to engine_script.
//
// Engine functions exposed to Lua report failures (a destroyed entity, a bad
// component field) by returning Result<T> — never by raising a Lua error
// from C++: raising unwinds with longjmp, which would skip the destructors
// of the C++ frames in between. A Result's error is pushed as a sentinel
// plus the message; checked_call, a plain C function wrapped around every
// fallible binding by bind(), turns that into a Lua error once the C++
// function has returned, positioned at the calling script line.

#include <expected>
#include <string>
#include <type_traits>
#include <utility>

#include "sol_config.hpp"

namespace engine::script::detail {

struct Failure {
    std::string message;
};

template <typename T>
struct Result {
    std::expected<T, Failure> value;

    Result(T v) : value(std::move(v)) {}                              // NOLINT: implicit success
    Result(std::unexpected<Failure> f) : value(std::move(f)) {}       // NOLINT: implicit failure
};

template <>
struct Result<void> {
    std::expected<void, Failure> value;

    Result() = default;
    Result(std::unexpected<Failure> f) : value(std::move(f)) {} // NOLINT: implicit failure
};

[[nodiscard]] inline std::unexpected<Failure> fail(std::string message) {
    return std::unexpected(Failure{std::move(message)});
}

// The address that marks "the binding failed" on the Lua stack.
inline const char k_failure_sentinel = 0;

void push_failure(lua_State* L, const Failure& failure);

// sol2 customization point (found by argument-dependent lookup).
template <typename T>
int sol_lua_push(sol::types<Result<T>> /*tag*/, lua_State* L, const Result<T>& result) {
    if (!result.value) {
        push_failure(L, result.value.error());
        return 2;
    }
    if constexpr (std::is_void_v<T>) {
        return 0;
    } else {
        return sol::stack::push(L, *result.value);
    }
}

// Calls upvalue 1 with the arguments; raises its failure as a Lua error.
int checked_call(lua_State* L);

// A Lua function value that runs `f` through checked_call.
template <typename F>
[[nodiscard]] sol::object bind(lua_State* L, F&& f) {
    sol::stack::push(L, std::forward<F>(f));
    lua_pushcclosure(L, &checked_call, 1);
    sol::object out(L, -1);
    lua_pop(L, 1);
    return out;
}

} // namespace engine::script::detail
