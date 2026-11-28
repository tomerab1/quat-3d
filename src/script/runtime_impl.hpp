#pragma once

// ScriptRuntime internals shared by script_runtime.cpp (sessions, instances,
// callbacks) and script_files.cpp (hot reload, statuses, property
// descriptions). Private to engine_script.

#include <cstdint>
#include <filesystem>
#include <memory>
#include <optional>
#include <string>
#include <unordered_map>
#include <vector>

#include <entt/entity/entity.hpp>

#include "engine/core/file_watcher.hpp"
#include "engine/input/actions.hpp"
#include "engine/script/script_runtime.hpp"
#include "lua_vm.hpp"
#include "script_api.hpp"

namespace engine::script {

namespace detail {

// One scripted entity's running script.
struct Instance {
    entt::entity  entity = entt::null;
    std::string   path;
    sol::table    self;
    sol::table    module; // the script's table (callbacks not overridden on self)
    std::uint32_t revision = 0; // Script::revision the overrides came from
    bool          started = false;
    bool          failed = false;
};

// A play session: the Lua state and everything referencing it. Members are
// destroyed bottom-up, so the references go before the state, and the
// state before the context its bindings point at.
struct Session {
    ApiContext                                  ctx;
    std::unique_ptr<LuaVm>                      vm;
    std::unordered_map<std::string, sol::table> modules;       // path -> loaded module
    std::unordered_map<std::string, std::string> module_errors; // path -> why it did not load
    std::vector<Instance>                       instances;     // creation order
    double                                      fixed_accumulator = 0.0;
    // Input (G5): the update's simulation sample, and the latch that hands
    // each fixed step the edges since the previous one.
    input::ActionSample                         update_sample;
    input::ActionSample                         fixed_sample;
    input::StepLatch                            fixed_latch;

    [[nodiscard]] Instance* find(entt::entity e);
};

// The scratch state of declared_properties().
struct Description {
    std::filesystem::file_time_type stamp{};
    std::uintmax_t                  size = 0;
    std::expected<std::vector<scene::ScriptProperty>, core::Error> properties;
};

// Reads the `properties` table of a module (sorted by name; values that are
// not number / boolean / string / vec3 are skipped).
[[nodiscard]] std::vector<scene::ScriptProperty> read_properties(const sol::table& module);

// Registers the whole script API into `vm`.
void register_api(LuaVm& vm, ApiContext& ctx);

} // namespace detail

struct ScriptRuntime::Impl {
    Options                                  options;
    ScriptServices                           services;
    std::unique_ptr<detail::Session>         session;
    std::vector<ScriptStatus>                statuses;
    std::optional<core::FileWatcher>         watcher;
    std::unordered_map<std::string, detail::Description> descriptions;
    // The default action map over services.input, when no service is given.
    std::optional<input::Actions>            fallback_actions;

    [[nodiscard]] ScriptStatus& status(std::string_view path);
    void set_error(std::string_view path, const std::string& message);

    // Loads (or returns the cached) module of `path` into the session.
    [[nodiscard]] std::expected<sol::table, std::string> module(const std::string& path);
    // Creates the instance's `self` (fresh table, declared defaults,
    // component overrides); false when the module did not load.
    [[nodiscard]] bool make_self(detail::Instance& inst, const scene::Script& script);
    void apply_overrides(detail::Instance& inst, const scene::Script& script);
    // Calls self[name](self, args...) for a started, healthy instance; an
    // error disables the instance.
    template <typename... Args>
    void invoke(detail::Instance& inst, const char* name, Args&&... args);
    void fail_instance(detail::Instance& inst, const std::string& message);
    // Runs on_destroy (when it started) and removes the instance.
    void end_instance(entt::entity e, bool call_destroy);
    void sync_instances();
    void flush_destroys();
    void count_instances();
    // Reloads a module and restarts its instances (session running).
    void reload_running(const std::string& path);
};

template <typename... Args>
void ScriptRuntime::Impl::invoke(detail::Instance& inst, const char* name, Args&&... args) {
    if (!inst.started || inst.failed || !session) return;
    // Raw lookups: no script metamethod runs outside a protected call.
    sol::object fn = inst.self.raw_get<sol::object>(name);
    if (fn.get_type() == sol::type::lua_nil) fn = inst.module.raw_get<sol::object>(name);
    if (fn.get_type() != sol::type::function) return;
    session->ctx.current_path = inst.path;
    auto ok = session->vm->call(fn, inst.self, std::forward<Args>(args)...);
    session->ctx.current_path.clear();
    if (!ok) fail_instance(inst, std::string(name) + ": " + ok.error());
}

} // namespace engine::script
