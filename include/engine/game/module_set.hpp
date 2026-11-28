#pragma once

// ModuleSet (ROADMAP G17.1): what a game's modules registered, loaded once at
// boot and used by the application each frame. Engine-side: games use
// module.hpp; this header is not part of the SDK's stable set.

#include <array>
#include <expected>
#include <filesystem>
#include <span>
#include <string>
#include <string_view>
#include <vector>

#include <nlohmann/json.hpp>

#include "engine/core/error.hpp"
#include "engine/game/module.hpp"
#include "engine/input/action_map.hpp"

namespace engine::input {
class Actions;
}

namespace engine::game {

// Remote namespaces the engine owns: a module method may not use them.
[[nodiscard]] std::span<const std::string_view> engine_method_namespaces();

struct ScheduledSystem {
    std::string name;
    Phase       phase = Phase::pre_physics;
    SystemFn    run = nullptr;
};

class ModuleSet {
public:
    struct Options {
        std::filesystem::path project_root{}; // action map paths are relative to it
    };

    ModuleSet() = default;

    // Runs each module's registration in order. Components go into
    // `components` (the process-wide registry in a game); on failure they are
    // unregistered again. The error lists every problem of every module.
    [[nodiscard]] static std::expected<ModuleSet, core::Error>
    load(std::span<const ModuleEntry> modules, scene::ComponentRegistry& components, Options options);

    [[nodiscard]] std::span<const std::string> names() const { return names_; }
    [[nodiscard]] bool empty() const { return names_.empty(); }

    // Systems of `phase`, in run order (each after the systems it names).
    [[nodiscard]] std::span<const ScheduledSystem> systems(Phase phase) const {
        return phases_[static_cast<std::size_t>(phase)];
    }
    void run(Phase phase, SystemContext& context, float dt) const;

    [[nodiscard]] std::span<const MethodDesc> methods() const { return methods_; }
    // Validates `params` against the method's schema, then calls it.
    [[nodiscard]] std::expected<nlohmann::json, core::Error>
    call(std::string_view method, SystemContext& context, const nlohmann::json& params) const;

    [[nodiscard]] std::span<const script::LuaModuleDesc> lua_modules() const { return lua_; }

    // The modules' action maps merged into `actions`' map.
    [[nodiscard]] std::expected<void, core::Error> apply_input(input::Actions& actions) const;
    [[nodiscard]] std::span<const input::ActionMap> action_maps() const { return action_maps_; }

    // Components the modules registered (their names), for unloading in tests.
    [[nodiscard]] std::span<const std::string> components() const { return components_; }

private:
    std::vector<std::string>                     names_;
    std::array<std::vector<ScheduledSystem>, k_phase_count> phases_{};
    std::vector<MethodDesc>                      methods_;
    std::vector<script::LuaModuleDesc>           lua_;
    std::vector<input::ActionMap>                action_maps_;
    std::vector<std::string>                     components_;
};

// Appends a module's map to `base`. Unlike input::merge_action_map, which
// lets a later map replace an action, a name `base` already has is an error
// naming it: two modules (or a module and the project) never silently
// override each other's actions.
[[nodiscard]] std::expected<void, core::Error> add_module_action_map(input::ActionMap& base,
                                                                const input::ActionMap& extra);

} // namespace engine::game
