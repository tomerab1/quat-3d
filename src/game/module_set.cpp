// ModuleSet — see include/engine/game/module_set.hpp.

#include "engine/game/module_set.hpp"

#include <algorithm>
#include <array>
#include <cstdio>
#include <utility>

#include "engine/core/profile.hpp"
#include "engine/game/json_schema.hpp"
#include "engine/input/actions.hpp"
#include "registrar_impl.hpp"

namespace engine::game {

namespace {

constexpr std::array<std::string_view, 26> k_engine_namespaces = {
    "asset",   "bt",      "camera",  "character", "debug",  "engine",  "environment",
    "events",  "game",    "input",   "material",  "nav",    "physics", "profile",
    "project", "render",  "rpc",     "scene",     "script", "session", "shader",
    "sim",     "terrain", "time",    "weather",   "world"};

// Orders one phase's systems so each runs after the ones it names (Kahn's
// algorithm, ties in registration order). Unknown names and cycles are errors.
std::expected<std::vector<ScheduledSystem>, std::string>
schedule(const std::vector<OwnedSystem>& systems) {
    const std::size_t n = systems.size();
    std::vector<std::vector<std::size_t>> before(n); // edges a -> b: a runs before b
    std::vector<std::size_t>              waiting(n, 0);
    for (std::size_t b = 0; b < n; ++b) {
        for (const std::string& name : systems[b].after) {
            const auto it = std::ranges::find_if(systems, [&](const OwnedSystem& s) { return s.name == name; });
            if (it == systems.end()) {
                return std::unexpected("system '" + systems[b].name + "' runs after '" + name +
                                       "', which is not a system of the " +
                                       std::string(phase_name(systems[b].phase)) + " phase");
            }
            before[static_cast<std::size_t>(it - systems.begin())].push_back(b);
            ++waiting[b];
        }
    }
    std::vector<ScheduledSystem> out;
    std::vector<bool>            done(n, false);
    while (out.size() < n) {
        std::size_t next = n;
        for (std::size_t i = 0; i < n && next == n; ++i) {
            if (!done[i] && waiting[i] == 0) next = i;
        }
        if (next == n) {
            std::string cycle;
            for (std::size_t i = 0; i < n; ++i) {
                if (!done[i]) cycle += (cycle.empty() ? "" : ", ") + systems[i].name;
            }
            return std::unexpected("systems ordered in a cycle with `after`: " + cycle);
        }
        done[next] = true;
        for (const std::size_t b : before[next]) --waiting[b];
        out.push_back({systems[next].name, systems[next].phase, systems[next].run});
    }
    return out;
}

} // namespace

std::span<const std::string_view> engine_method_namespaces() { return k_engine_namespaces; }

std::expected<ModuleSet, core::Error> ModuleSet::load(std::span<const ModuleEntry> modules,
                                                      scene::ComponentRegistry& components,
                                                      Options options) {
    ModuleSet set;
    std::vector<std::string>  problems;
    std::vector<OwnedSystem>  all_systems;
    for (const ModuleEntry& entry : modules) {
        const std::string name(entry.name);
        const auto problem = [&](const std::string& what) {
            problems.push_back("module '" + name + "': " + what);
        };
        if (std::ranges::find(set.names_, name) != set.names_.end()) {
            problem("listed twice");
            continue;
        }
        set.names_.push_back(name);
        if (entry.register_fn == nullptr) {
            problem("no registration function");
            continue;
        }
        Registrar registrar(name, components);
        registrar.impl().project_root = options.project_root;
        if (auto ok = entry.register_fn(registrar); !ok) problem(ok.error().message);
        Registrar::Impl& r = registrar.impl();
        for (const std::string& e : r.errors) problem(e);
        for (std::string& c : r.component_names) set.components_.push_back(std::move(c));
        for (OwnedSystem& s : r.systems) {
            if (std::ranges::any_of(all_systems, [&](const OwnedSystem& o) { return o.name == s.name; })) {
                problem("system '" + s.name + "' is already registered");
                continue;
            }
            all_systems.push_back(std::move(s));
        }
        for (MethodDesc& m : r.methods) {
            if (std::ranges::any_of(set.methods_, [&](const MethodDesc& o) { return o.name == m.name; })) {
                problem("method '" + m.name + "' is already registered");
                continue;
            }
            set.methods_.push_back(std::move(m));
        }
        for (script::LuaModuleDesc& l : r.lua_modules) {
            if (std::ranges::any_of(set.lua_, [&](const auto& o) { return o.name == l.name; })) {
                problem("lua module '" + l.name + "' is already registered");
                continue;
            }
            set.lua_.push_back(std::move(l));
        }
        for (input::ActionMap& m : r.action_maps) set.action_maps_.push_back(std::move(m));
    }
    for (std::size_t p = 0; p < k_phase_count; ++p) {
        std::vector<OwnedSystem> phase;
        for (const OwnedSystem& s : all_systems) {
            if (static_cast<std::size_t>(s.phase) == p) phase.push_back(s);
        }
        auto ordered = schedule(phase);
        if (!ordered) {
            problems.push_back(ordered.error());
            continue;
        }
        set.phases_[p] = std::move(*ordered);
    }
    // The maps must merge with each other (the project's map is checked at apply).
    input::ActionMap merged;
    for (const input::ActionMap& m : set.action_maps_) {
        if (auto ok = add_module_action_map(merged, m); !ok) problems.push_back(ok.error().message);
    }
    if (!problems.empty()) {
        for (const std::string& c : set.components_) (void)components.remove(c);
        std::string text = "game modules failed to register:";
        for (const std::string& p : problems) text += "\n  " + p;
        return std::unexpected(core::Error{std::move(text)});
    }
    return set;
}

void ModuleSet::run(Phase phase, SystemContext& context, float dt) const {
    for (const ScheduledSystem& s : systems(phase)) {
        QUAT_ZONE_C("module_system", core::ProfileCategory::system);
        s.run(context, dt);
    }
}

std::expected<nlohmann::json, core::Error> ModuleSet::call(std::string_view method,
                                                           SystemContext& context,
                                                           const nlohmann::json& params) const {
    const auto it = std::ranges::find_if(methods_, [&](const MethodDesc& m) { return m.name == method; });
    if (it == methods_.end()) {
        return std::unexpected(core::Error{"unknown method '" + std::string(method) + "'"});
    }
    if (auto ok = validate_json(it->params_schema, params); !ok) return std::unexpected(ok.error());
    return it->handler(context, params);
}

std::expected<void, core::Error> ModuleSet::apply_input(input::Actions& actions) const {
    if (action_maps_.empty()) return {};
    input::ActionMap map = actions.defaults();
    for (const input::ActionMap& m : action_maps_) {
        if (auto ok = add_module_action_map(map, m); !ok) return ok;
    }
    actions.set_map(std::move(map));
    return {};
}

std::expected<void, core::Error> add_module_action_map(input::ActionMap& base, const input::ActionMap& extra) {
    for (const input::ContextDesc& c : extra.contexts) {
        if (base.find_context(c.name) >= 0) {
            return std::unexpected(core::Error{"input context '" + c.name + "' is defined twice"});
        }
    }
    for (const input::ActionDesc& a : extra.actions) {
        if (base.find_action(a.name) >= 0) {
            return std::unexpected(core::Error{"input action '" + a.name + "' is defined twice"});
        }
    }
    if (base.contexts.size() + extra.contexts.size() > input::k_max_contexts ||
        base.actions.size() + extra.actions.size() > input::k_max_actions) {
        return std::unexpected(core::Error{"input actions: the merged map exceeds " +
                                           std::to_string(input::k_max_actions) + " actions or " +
                                           std::to_string(input::k_max_contexts) + " contexts"});
    }
    base.contexts.insert(base.contexts.end(), extra.contexts.begin(), extra.contexts.end());
    base.actions.insert(base.actions.end(), extra.actions.begin(), extra.actions.end());
    return {};
}

} // namespace engine::game
