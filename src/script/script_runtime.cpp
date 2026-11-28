// ScriptRuntime: play sessions, instances and callbacks — see
// include/engine/script/script_runtime.hpp. Files, hot reload and statuses:
// script_files.cpp.

#include "engine/script/script_runtime.hpp"

#include <algorithm>
#include <cmath>
#include <unordered_set>
#include <utility>

#include <entt/entity/registry.hpp>

#include "engine/core/log.hpp"
#include "engine/core/profile.hpp"
#include "engine/physics/physics_world.hpp"
#include "engine/scene/scene.hpp"
#include "engine/script/script_source.hpp"
#include "runtime_impl.hpp"

namespace engine::script {

namespace {

constexpr float       k_fixed_dt = 1.0F / 60.0F;
constexpr int         k_max_fixed_steps = 5;
constexpr int         k_max_destroy_rounds = 16;
constexpr std::size_t k_max_subtree = 100'000;

// `e` and its descendants, parents first.
[[nodiscard]] std::vector<entt::entity> subtree(const entt::registry& r, entt::entity root) {
    std::vector<entt::entity> out{root};
    for (std::size_t i = 0; i < out.size() && out.size() < k_max_subtree; ++i) {
        if (const auto* ch = r.try_get<scene::Children>(out[i])) {
            for (const entt::entity c : ch->entities) {
                if (r.valid(c) && std::ranges::find(out, c) == out.end()) out.push_back(c);
            }
        }
    }
    return out;
}

[[nodiscard]] sol::object copy_value(lua_State* L, const sol::object& v) {
    if (v.is<glm::vec3>()) return sol::make_object(L, v.as<glm::vec3>());
    if (v.is<glm::quat>()) return sol::make_object(L, v.as<glm::quat>());
    return v;
}

} // namespace

namespace detail {

Instance* Session::find(entt::entity e) {
    const auto it = std::ranges::find(instances, e, &Instance::entity);
    return it != instances.end() ? &*it : nullptr;
}

void register_api(LuaVm& vm, ApiContext& ctx) {
    register_math(vm);
    register_entity(vm, ctx);
    register_scene(vm, ctx);
    register_services(vm, ctx);
    register_input(vm, ctx);
    register_terrain(vm, ctx);
}

} // namespace detail

ScriptRuntime::ScriptRuntime() : ScriptRuntime(Options{}) {}

ScriptRuntime::ScriptRuntime(Options options) : impl_(std::make_unique<Impl>()) {
    impl_->options = std::move(options);
    if (impl_->options.watch && !impl_->options.scripts_root.empty()) {
        core::FileWatcher::Options w;
        w.directories.push_back(impl_->options.scripts_root);
        std::error_code ec;
        for (auto it = std::filesystem::recursive_directory_iterator(impl_->options.scripts_root, ec);
             !ec && it != std::filesystem::recursive_directory_iterator(); it.increment(ec)) {
            if (it->is_directory(ec)) w.directories.push_back(it->path());
        }
        w.extensions = {".lua"};
        if (auto watcher = core::FileWatcher::create(std::move(w)); watcher) {
            impl_->watcher.emplace(std::move(*watcher));
        } else {
            core::log(core::LogLevel::warn, "script", "script hot reload is off: {}",
                      watcher.error().message);
        }
    }
}

ScriptRuntime::~ScriptRuntime() = default;
ScriptRuntime::ScriptRuntime(ScriptRuntime&&) noexcept = default;
ScriptRuntime& ScriptRuntime::operator=(ScriptRuntime&&) noexcept = default;

std::expected<void, core::Error> ScriptRuntime::start(scene::Scene& scene) {
    QUAT_ZONE_N("script_start"); // VM + API registration (modules load lazily)
    stop();
    detail::VmLimits limits;
    limits.memory_bytes = impl_->options.limits.memory_bytes;
    limits.call_memory_bytes = impl_->options.limits.call_memory_bytes;
    limits.call_instructions = impl_->options.limits.call_instructions;
    auto session = std::make_unique<detail::Session>();
    auto vm = detail::LuaVm::create(impl_->options.scripts_root, limits);
    if (!vm) return std::unexpected(vm.error());
    session->vm = std::move(*vm);
    session->ctx.scene = &scene;
    session->ctx.services = impl_->services;
    session->ctx.fixed_dt = k_fixed_dt;
    detail::Session* s = session.get();
    session->ctx.instance_of = [s](entt::entity e) -> sol::object {
        if (const detail::Instance* inst = s->find(e); inst != nullptr && inst->self.valid()) {
            return inst->self;
        }
        return sol::make_object(s->vm->state(), sol::lua_nil);
    };
    detail::register_api(*session->vm, session->ctx);
    impl_->session = std::move(session);
    return {};
}

void ScriptRuntime::stop() {
    impl_->session.reset();
    for (ScriptStatus& s : impl_->statuses) {
        s.instances = 0;
        s.failed = 0;
    }
}

bool ScriptRuntime::running() const { return impl_->session != nullptr; }

void ScriptRuntime::set_services(ScriptServices services) {
    impl_->services = std::move(services);
    if (impl_->session) impl_->session->ctx.services = impl_->services;
}

const std::filesystem::path& ScriptRuntime::scripts_root() const { return impl_->options.scripts_root; }

void ScriptRuntime::update(float dt) {
    detail::Session* s = impl_->session.get();
    if (s == nullptr || !(dt > 0.0F)) return; // paused: scripts hold too
    QUAT_ZONE_N("script_update");
    s->ctx.dt = dt;
    s->ctx.elapsed_s += dt;
    ++s->ctx.frame;

    // Input (G5): one simulation sample per update; each fixed step sees
    // the edges since the previous fixed step exactly once.
    input::Actions* actions = impl_->services.actions;
    if (actions == nullptr) {
        if (!impl_->fallback_actions) impl_->fallback_actions.emplace();
        actions = &*impl_->fallback_actions;
        if (impl_->services.input != nullptr) actions->update(*impl_->services.input, dt);
    }
    s->update_sample = actions->take_sim_sample();
    s->fixed_latch.add(s->update_sample);
    s->ctx.action_service = actions;
    s->ctx.action_sample = &s->update_sample; // on_start

    impl_->sync_instances();

    s->fixed_accumulator += dt;
    {
        QUAT_ZONE_N("script_on_fixed_update");
        int steps = 0;
        while (s->fixed_accumulator >= k_fixed_dt && steps < k_max_fixed_steps) {
            s->fixed_sample = s->fixed_latch.take(actions->map());
            s->ctx.action_sample = &s->fixed_sample;
            for (std::size_t i = 0; i < s->instances.size(); ++i) {
                impl_->invoke(s->instances[i], "on_fixed_update", k_fixed_dt);
            }
            s->fixed_accumulator -= k_fixed_dt;
            ++steps;
        }
        if (steps == k_max_fixed_steps) {
            s->fixed_accumulator = std::fmod(s->fixed_accumulator, k_fixed_dt);
        }
    }

    {
        QUAT_ZONE_N("script_on_update");
        s->ctx.action_sample = &s->update_sample;
        for (std::size_t i = 0; i < s->instances.size(); ++i) {
            impl_->invoke(s->instances[i], "on_update", dt);
        }
    }
    impl_->flush_destroys();
    impl_->count_instances();
}

void ScriptRuntime::dispatch_triggers(const physics::PhysicsWorld& world) {
    detail::Session* s = impl_->session.get();
    if (s == nullptr) return;
    QUAT_ZONE_N("script_triggers");
    const entt::registry& r = s->ctx.scene->registry();
    const auto entity_of = [&](std::uint32_t body) {
        const std::uint32_t owner = world.body_owner(body);
        const auto e = static_cast<entt::entity>(owner);
        return owner != physics::PhysicsWorld::no_owner && r.valid(e) ? e : entt::null;
    };
    // Copy: callbacks may not touch the physics world's event list, but be safe.
    const std::vector<physics::PhysicsWorld::TriggerEvent> events(world.trigger_events().begin(),
                                                                  world.trigger_events().end());
    for (const physics::PhysicsWorld::TriggerEvent& ev : events) {
        const entt::entity sensor = entity_of(ev.trigger);
        const entt::entity other = entity_of(ev.other);
        if (sensor == entt::null) continue;
        const char* name = ev.entered ? "on_trigger_enter" : "on_trigger_exit";
        if (detail::Instance* inst = s->find(sensor)) {
            if (other != entt::null) {
                impl_->invoke(*inst, name, detail::ScriptEntity{other});
            } else {
                impl_->invoke(*inst, name, sol::lua_nil);
            }
        }
        if (other == entt::null) continue;
        if (detail::Instance* inst = s->find(other)) {
            impl_->invoke(*inst, name, detail::ScriptEntity{sensor});
        }
    }
    impl_->flush_destroys();
}

// ---- Impl: instances ------------------------------------------------------

std::expected<sol::table, std::string> ScriptRuntime::Impl::module(const std::string& path) {
    detail::Session& s = *session;
    if (const auto it = s.modules.find(path); it != s.modules.end()) return it->second;
    if (const auto it = s.module_errors.find(path); it != s.module_errors.end()) {
        return std::unexpected(it->second);
    }
    QUAT_ZONE_N("script_load_module"); // first use of a file: read + compile + run
    std::expected<sol::table, std::string> loaded = std::unexpected(std::string());
    if (auto file = script_file(options.scripts_root, path); !file) {
        loaded = std::unexpected(file.error().message);
    } else if (auto source = read_script(*file); !source) {
        loaded = std::unexpected(source.error().message);
    } else {
        s.ctx.current_path = path;
        loaded = s.vm->load_module(path, *source);
        s.ctx.current_path.clear();
    }
    if (!loaded) {
        s.module_errors[path] = loaded.error();
        set_error(path, loaded.error());
        return loaded;
    }
    s.modules[path] = *loaded;
    return loaded;
}

bool ScriptRuntime::Impl::make_self(detail::Instance& inst, const scene::Script& script) {
    auto mod = module(inst.path);
    if (!mod) return false;
    lua_State* L = session->vm->state();
    inst.module = *mod;
    inst.self = sol::table(L, sol::create);
    sol::table mt(L, sol::create);
    mt["__index"] = *mod;
    mt["__metatable"] = false; // scripts cannot swap the instance's metatable
    inst.self[sol::metatable_key] = mt;
    inst.self["entity"] = detail::ScriptEntity{inst.entity};
    const sol::object declared = mod->raw_get<sol::object>("properties");
    if (declared.get_type() == sol::type::table) {
        for (const auto& [key, value] : declared.as<sol::table>()) {
            if (key.get_type() == sol::type::string && key.as<std::string>() != "entity") {
                inst.self[key] = copy_value(L, value);
            }
        }
    }
    apply_overrides(inst, script);
    return true;
}

void ScriptRuntime::Impl::apply_overrides(detail::Instance& inst, const scene::Script& script) {
    inst.revision = script.revision;
    if (!inst.self.valid()) return;
    for (const scene::ScriptProperty& p : script.properties) {
        switch (p.type) {
        case scene::ScriptValueType::number: inst.self[p.name] = p.number; break;
        case scene::ScriptValueType::boolean: inst.self[p.name] = p.boolean; break;
        case scene::ScriptValueType::string: inst.self[p.name] = p.text; break;
        case scene::ScriptValueType::vec3: inst.self[p.name] = p.vec; break;
        }
    }
}

void ScriptRuntime::Impl::fail_instance(detail::Instance& inst, const std::string& message) {
    inst.failed = true;
    const entt::registry& r = session->ctx.scene->registry();
    const std::string who = detail::describe_entity(r, inst.entity);
    core::log(core::LogLevel::error, "script", "{} (entity {}) stopped: {}", inst.path, who, message);
    set_error(inst.path, message);
}

void ScriptRuntime::Impl::end_instance(entt::entity e, bool call_destroy) {
    detail::Session& s = *session;
    const auto it = std::ranges::find(s.instances, e, &detail::Instance::entity);
    if (it == s.instances.end()) return;
    if (call_destroy) invoke(*it, "on_destroy");
    // invoke() cannot add or remove instances: `it` is still valid.
    s.instances.erase(it);
}

void ScriptRuntime::Impl::sync_instances() {
    detail::Session& s = *session;
    entt::registry& r = s.ctx.scene->registry();
    // End instances whose entity or script went away.
    // on_destroy runs even when the entity is already gone (destroyed by the
    // editor or a remote client): self.entity:valid() tells the script.
    std::vector<entt::entity> stale;
    for (detail::Instance& inst : s.instances) {
        const auto* sc = r.valid(inst.entity) ? r.try_get<scene::Script>(inst.entity) : nullptr;
        if (sc == nullptr || !sc->enabled || sc->path != inst.path) {
            stale.push_back(inst.entity);
        } else if (sc->revision != inst.revision && !inst.failed) {
            apply_overrides(inst, *sc);
        }
    }
    for (const entt::entity e : stale) end_instance(e, true);

    // New instances. Collected first: loading a module runs its top-level
    // code, which may spawn scripted entities into the view.
    std::vector<entt::entity> fresh;
    for (const auto [e, sc] : r.view<scene::Script>().each()) {
        if (sc.enabled && !sc.path.empty() && s.find(e) == nullptr) fresh.push_back(e);
    }
    for (const entt::entity e : fresh) {
        const auto* sc = r.valid(e) ? r.try_get<scene::Script>(e) : nullptr;
        if (sc == nullptr || s.find(e) != nullptr) continue;
        const scene::Script script = *sc; // a copy: module code may touch the registry
        detail::Instance inst;
        inst.entity = e;
        inst.path = script.path;
        if (!make_self(inst, script)) inst.failed = true; // the load error is in the status
        s.instances.push_back(std::move(inst));
    }
    // Start them (new instances, and those restarted by a hot reload).
    for (std::size_t i = 0; i < s.instances.size(); ++i) {
        detail::Instance& inst = s.instances[i];
        if (inst.started || inst.failed) continue;
        inst.started = true;
        invoke(inst, "on_start");
    }
}

void ScriptRuntime::Impl::flush_destroys() {
    detail::Session& s = *session;
    entt::registry& r = s.ctx.scene->registry();
    for (int round = 0; round < k_max_destroy_rounds && !s.ctx.pending_destroy.empty(); ++round) {
        const std::vector<entt::entity> doomed = std::exchange(s.ctx.pending_destroy, {});
        for (const entt::entity root : doomed) {
            if (!r.valid(root)) continue;
            const std::vector<entt::entity> tree = subtree(r, root);
            for (const entt::entity e : tree) end_instance(e, true);
            if (!r.valid(root)) continue; // an on_destroy got there first
            (void)s.ctx.scene->set_parent(root, entt::null);
            for (auto it = tree.rbegin(); it != tree.rend(); ++it) {
                if (r.valid(*it)) r.destroy(*it);
            }
        }
    }
    s.ctx.pending_destroy.clear();
}

void ScriptRuntime::Impl::count_instances() {
    for (ScriptStatus& st : statuses) {
        st.instances = 0;
        st.failed = 0;
    }
    if (!session) return;
    for (const detail::Instance& inst : session->instances) {
        ScriptStatus& st = status(inst.path);
        ++(inst.failed ? st.failed : st.instances);
    }
}

} // namespace engine::script
