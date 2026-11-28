// Registrar — see include/engine/game/module.hpp.

#include "engine/game/module.hpp"

#include <algorithm>
#include <utility>

#include "engine/core/path_confine.hpp"
#include "engine/game/json_schema.hpp"
#include "engine/game/module_set.hpp"
#include "engine/input/actions.hpp"
#include "engine/scene/scene.hpp"
#include "registrar_impl.hpp"

namespace engine::game {

namespace {

// "cozy.generate", "cozy_wall": lowercase words joined by '.' or '_'.
[[nodiscard]] bool valid_name(std::string_view name, bool dotted) {
    if (name.empty() || name.size() > 64 || name.front() < 'a' || name.front() > 'z') return false;
    char previous = 0;
    for (const char c : name) {
        const bool word = (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') || c == '_';
        if (!word && !(dotted && c == '.')) return false;
        if (c == '.' && previous == '.') return false;
        previous = c;
    }
    return name.back() != '.';
}

[[nodiscard]] bool lua_identifier(std::string_view name) {
    if (name.empty() || name.size() > 64 || (name.front() >= '0' && name.front() <= '9')) return false;
    return std::ranges::all_of(name, [](char c) {
        return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') || c == '_';
    });
}

} // namespace

std::string_view phase_name(Phase phase) {
    switch (phase) {
    case Phase::input: return "input";
    case Phase::pre_physics: return "pre_physics";
    case Phase::post_physics: return "post_physics";
    case Phase::post_transform: return "post_transform";
    case Phase::pre_render: return "pre_render";
    }
    return "unknown";
}

entt::registry& SystemContext::registry() { return scene_->registry(); }

Registrar::Registrar(std::string module_name, scene::ComponentRegistry& components)
    : module_(std::move(module_name)), impl_(std::make_unique<Impl>()) {
    impl_->components = &components;
}

Registrar::~Registrar() = default;

void Registrar::fail(std::string message) { impl_->errors.push_back(std::move(message)); }

void Registrar::add_component(scene::ComponentCodec codec) {
    const std::string name = codec.name;
    if (auto ok = impl_->components->add(std::move(codec)); !ok) {
        fail("component: " + ok.error().message);
        return;
    }
    impl_->component_names.push_back(name);
}

void Registrar::system(const SystemDesc& desc) {
    const std::string name(desc.name);
    if (!valid_name(desc.name, true)) {
        fail("system '" + name + "': a name is lowercase letters, digits, '_' and '.'");
        return;
    }
    if (desc.run == nullptr) {
        fail("system '" + name + "': no function to run");
        return;
    }
    if (static_cast<std::size_t>(desc.phase) >= k_phase_count) {
        fail("system '" + name + "': unknown phase");
        return;
    }
    OwnedSystem s{.name = name, .phase = desc.phase, .run = desc.run, .after = {}};
    for (const std::string_view a : desc.after) s.after.emplace_back(a);
    impl_->systems.push_back(std::move(s));
}

void Registrar::method(MethodDesc desc) {
    const std::string& name = desc.name;
    if (!valid_name(name, true) || name.find('.') == std::string::npos) {
        fail("method '" + name + "': a name is \"<namespace>.<verb>\" in lowercase letters, "
             "digits and '_'");
        return;
    }
    const std::string_view ns = std::string_view(name).substr(0, name.find('.'));
    for (const std::string_view reserved : engine_method_namespaces()) {
        if (ns == reserved) {
            fail("method '" + name + "': the namespace '" + std::string(ns) +
                 "' belongs to the engine; use the game's own");
            return;
        }
    }
    if (!desc.handler) {
        fail("method '" + name + "': no handler");
        return;
    }
    if (auto ok = check_schema(desc.params_schema); !ok) {
        fail("method '" + name + "': params_schema " + ok.error().message);
        return;
    }
    impl_->methods.push_back(std::move(desc));
}

void Registrar::lua_module(script::LuaModuleDesc desc) {
    if (!valid_name(desc.name, false)) {
        fail("lua module '" + desc.name + "': a name is lowercase letters, digits and '_'");
        return;
    }
    for (const script::LuaFunctionDesc& f : desc.functions) {
        if (!lua_identifier(f.name) || !f.fn) {
            fail("lua module '" + desc.name + "': function '" + f.name +
                 "' needs a Lua identifier and a function");
            return;
        }
    }
    impl_->lua_modules.push_back(std::move(desc));
}

void Registrar::input_actions(std::string_view project_path) {
    const std::string where = "input actions '" + std::string(project_path) + "'";
    auto path = core::confine_to_root(impl_->project_root, project_path);
    if (!path) {
        fail(where + ": " + path.error().message);
        return;
    }
    auto map = input::load_action_map_file(*path);
    if (!map) {
        fail(where + ": " + map.error().message);
        return;
    }
    impl_->action_maps.push_back(std::move(*map));
}

void Registrar::ui_screen(UiScreenDesc desc) {
    fail("ui screen '" + desc.name + "': UI screens need ROADMAP G6.1, which this engine "
         "version does not have");
}

void Registrar::save_chunk(SaveChunkDesc desc) {
    fail("save chunk '" + desc.name + "': save chunks need ROADMAP G11.1, which this engine "
         "version does not have");
}

} // namespace engine::game
