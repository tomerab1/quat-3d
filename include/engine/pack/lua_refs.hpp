#pragma once

// Static scan of Lua sources for the files a script names (ROADMAP P4): the
// packager follows `require("lib.util")` and string literals such as
// "coin_collector/coin.lua" (a script spawned at runtime) or
// "assets/meshes/crate.glb" (scene.spawn_model), so a game ships what its
// scripts load without listing it by hand. A lexer, not a parser: comments
// are skipped, short and long strings are decoded, and `require` followed by
// a string (with or without parentheses) is recognised.

#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

namespace engine::pack {

struct LuaLiteral {
    std::string   text;     // decoded string contents
    std::uint32_t line = 0; // 1-based line of the literal
    bool          required = false; // the argument of a require call
};

// Every string literal in `source`, in order. Malformed input (an
// unterminated string) ends the scan; what was found so far is returned.
[[nodiscard]] std::vector<LuaLiteral> lua_string_literals(std::string_view source);

} // namespace engine::pack
