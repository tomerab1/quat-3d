#pragma once

// Lua modules from native code (ROADMAP G17.1): a game module offers Lua
// scripts a table of functions, reached with require("<name>"). The binding
// is Lua-free: arguments arrive as one JSON array (every Lua argument
// converted as the codec converts tables), the result goes back as a Lua
// value, and an error becomes a Lua error with the message. Scripts stay in
// the sandbox: a module function is called with the script's instruction and
// memory budget, like the engine's own bindings.

#include <expected>
#include <functional>
#include <string>
#include <vector>

#include <nlohmann/json.hpp>

#include "engine/core/error.hpp"

namespace engine::script {

using LuaFunction =
    std::function<std::expected<nlohmann::json, core::Error>(const nlohmann::json& args)>;

struct LuaFunctionDesc {
    std::string name{}; // a Lua identifier
    LuaFunction fn{};
};

struct LuaModuleDesc {
    std::string                  name{}; // require("<name>"): lowercase letters, digits, '_'
    std::vector<LuaFunctionDesc> functions{};
};

} // namespace engine::script
