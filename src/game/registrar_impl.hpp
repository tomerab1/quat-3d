#pragma once

// Private to engine_game: what one module registered (Registrar::Impl).

#include <string>
#include <vector>

#include "engine/game/module.hpp"
#include "engine/input/action_map.hpp"

namespace engine::game {

struct OwnedSystem {
    std::string              name;
    Phase                    phase = Phase::pre_physics;
    SystemFn                 run = nullptr;
    std::vector<std::string> after;
};

struct Registrar::Impl {
    scene::ComponentRegistry*          components = nullptr;
    std::filesystem::path              project_root;
    std::vector<std::string>           errors;
    std::vector<std::string>           component_names;
    std::vector<OwnedSystem>           systems;
    std::vector<MethodDesc>            methods;
    std::vector<script::LuaModuleDesc> lua_modules;
    std::vector<input::ActionMap>      action_maps;
};

} // namespace engine::game
