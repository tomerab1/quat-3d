// ScriptRuntime: script files — hot reload, statuses and the declared
// properties the inspector shows. See include/engine/script/script_runtime.hpp.

#include <algorithm>
#include <utility>

#include <entt/entity/registry.hpp>

#include "engine/core/log.hpp"
#include "engine/core/profile.hpp"
#include "engine/core/utf8_path.hpp"
#include "engine/scene/scene.hpp"
#include "engine/scene/script_component.hpp"
#include "engine/script/script_source.hpp"
#include "runtime_impl.hpp"

namespace engine::script {

namespace {

namespace fs = std::filesystem;

// The describe state: small budgets, no scene.
constexpr detail::VmLimits k_describe_limits{16U << 20, 16U << 20, 1'000'000};

struct FileStamp {
    fs::file_time_type time{};
    std::uintmax_t     size = 0;
    bool               exists = false;
};

[[nodiscard]] FileStamp stamp_of(const fs::path& file) {
    std::error_code ec;
    FileStamp s;
    s.time = fs::last_write_time(file, ec);
    if (ec) return s;
    s.size = fs::file_size(file, ec);
    s.exists = !ec;
    return s;
}

} // namespace

namespace detail {

std::vector<scene::ScriptProperty> read_properties(const sol::table& module) {
    std::vector<scene::ScriptProperty> out;
    const sol::object declared = module.raw_get<sol::object>("properties");
    if (declared.get_type() != sol::type::table) return out;
    for (const auto& [key, value] : declared.as<sol::table>()) {
        if (key.get_type() != sol::type::string) continue;
        scene::ScriptProperty p;
        p.name = key.as<std::string>();
        if (!scene::validate_property_name(p.name)) continue;
        switch (value.get_type()) {
        case sol::type::number:
            p.type = scene::ScriptValueType::number;
            p.number = value.as<double>();
            break;
        case sol::type::boolean:
            p.type = scene::ScriptValueType::boolean;
            p.boolean = value.as<bool>();
            break;
        case sol::type::string:
            p.type = scene::ScriptValueType::string;
            p.text = value.as<std::string>();
            break;
        default:
            if (!value.is<glm::vec3>()) continue;
            p.type = scene::ScriptValueType::vec3;
            p.vec = value.as<glm::vec3>();
            break;
        }
        out.push_back(std::move(p));
        if (out.size() >= scene::k_max_script_properties) break;
    }
    std::ranges::sort(out, {}, &scene::ScriptProperty::name);
    return out;
}

} // namespace detail

ScriptStatus& ScriptRuntime::Impl::status(std::string_view path) {
    const auto it = std::ranges::find(statuses, path, &ScriptStatus::path);
    if (it != statuses.end()) return *it;
    ScriptStatus s;
    s.path = std::string(path);
    statuses.push_back(std::move(s));
    std::ranges::sort(statuses, {}, &ScriptStatus::path);
    return *std::ranges::find(statuses, path, &ScriptStatus::path);
}

void ScriptRuntime::Impl::set_error(std::string_view path, const std::string& message) {
    ScriptStatus& s = status(path);
    s.state = ScriptStatus::State::error;
    s.message = message;
}

const std::vector<ScriptStatus>& ScriptRuntime::statuses() const { return impl_->statuses; }

void ScriptRuntime::poll_changes() {
    if (!impl_->watcher) return;
    for (const core::FileChange& change : impl_->watcher->poll()) {
        const std::string rel =
            core::path_to_generic_utf8(
                change.path.lexically_relative(impl_->options.scripts_root));
        if (scene::validate_script_path(rel)) reload(rel);
    }
}

void ScriptRuntime::reload(std::string_view path_view) {
    QUAT_ZONE_N("script_reload");
    const std::string path(path_view);
    impl_->descriptions.erase(path);
    if (impl_->session) {
        impl_->reload_running(path);
        return;
    }
    // Edit mode: only check the file, so the editor shows errors before Play.
    auto file = script_file(impl_->options.scripts_root, path);
    if (!file) return;
    auto source = read_script(*file);
    if (!source) {
        if (const auto it = std::ranges::find(impl_->statuses, path, &ScriptStatus::path);
            it != impl_->statuses.end()) {
            impl_->set_error(path, source.error().message);
        }
        return;
    }
    ScriptStatus& s = impl_->status(path);
    if (auto ok = check_syntax(*source, path); !ok) {
        s.state = ScriptStatus::State::error;
        s.message = ok.error().message;
        core::log(core::LogLevel::error, "script", "{}", ok.error().message);
    } else {
        s.state = ScriptStatus::State::ok;
        s.message.clear();
    }
}

void ScriptRuntime::Impl::reload_running(const std::string& path) {
    detail::Session& s = *session;
    lua_State* L = s.vm->state();
    // A module loaded through require(): drop it from the cache and reload
    // every script (they may hold on to it).
    bool required = false;
    lua_getfield(L, LUA_REGISTRYINDEX, "quat.loaded");
    if (lua_type(L, -1) == LUA_TTABLE) {
        required = lua_getfield(L, -1, path.c_str()) != LUA_TNIL;
        lua_pop(L, 1);
        if (required) {
            lua_pushnil(L);
            lua_setfield(L, -2, path.c_str());
        }
    }
    lua_pop(L, 1);
    if (required) {
        std::vector<std::string> loaded;
        for (const auto& [p, m] : s.modules) loaded.push_back(p);
        for (const std::string& p : loaded) {
            if (p != path) reload_running(p);
        }
        if (!s.modules.contains(path)) return; // only a library
    }

    auto file = script_file(options.scripts_root, path);
    std::expected<sol::table, std::string> fresh = std::unexpected(std::string());
    if (!file) {
        fresh = std::unexpected(file.error().message);
    } else if (auto source = read_script(*file); !source) {
        fresh = std::unexpected(source.error().message);
    } else {
        s.ctx.current_path = path;
        fresh = s.vm->load_module(path, *source);
        s.ctx.current_path.clear();
    }
    if (!fresh) {
        const bool running = s.modules.contains(path);
        core::log(core::LogLevel::error, "script", "{}: reload failed{}: {}", path,
                  running ? " (the previous version keeps running)" : "", fresh.error());
        set_error(path, fresh.error());
        if (!running) s.module_errors[path] = fresh.error();
        return;
    }
    s.modules[path] = *fresh;
    s.module_errors.erase(path);
    ScriptStatus& st = status(path);
    st.state = ScriptStatus::State::ok;
    st.message.clear();
    ++st.reloads;
    core::log(core::LogLevel::info, "script", "{}: reloaded", path);

    // Restart its instances with the new code.
    const entt::registry& r = s.ctx.scene->registry();
    for (detail::Instance& inst : s.instances) {
        if (inst.path != path) continue;
        if (inst.started && !inst.failed) invoke(inst, "on_destroy");
        const auto* sc = r.valid(inst.entity) ? r.try_get<scene::Script>(inst.entity) : nullptr;
        inst.started = false;
        inst.failed = false;
        inst.self = sol::table();
        if (sc == nullptr || !make_self(inst, *sc)) inst.failed = sc != nullptr;
    }
    count_instances();
}

std::expected<std::vector<scene::ScriptProperty>, core::Error>
ScriptRuntime::declared_properties(std::string_view path_view) {
    const std::string path(path_view);
    if (auto ok = scene::validate_script_path(path); !ok) return std::unexpected(ok.error());
    if (impl_->session) {
        if (const auto it = impl_->session->modules.find(path); it != impl_->session->modules.end()) {
            return detail::read_properties(it->second);
        }
    }
    auto file = script_file(impl_->options.scripts_root, path);
    if (!file) return std::unexpected(file.error());
    const FileStamp stamp = stamp_of(*file);
    if (!stamp.exists) return std::unexpected(core::Error{"assets/scripts/" + path + " does not exist"});
    if (const auto it = impl_->descriptions.find(path);
        it != impl_->descriptions.end() && it->second.stamp == stamp.time &&
        it->second.size == stamp.size) {
        return it->second.properties;
    }
    detail::Description d;
    d.stamp = stamp.time;
    d.size = stamp.size;
    d.properties = [&]() -> std::expected<std::vector<scene::ScriptProperty>, core::Error> {
        auto source = read_script(*file);
        if (!source) return std::unexpected(source.error());
        detail::ApiContext ctx; // no scene: top-level code cannot touch it; outlives the state
        auto vm = detail::LuaVm::create(impl_->options.scripts_root, k_describe_limits);
        if (!vm) return std::unexpected(vm.error());
        detail::register_api(**vm, ctx);
        auto module = (*vm)->load_module(path, *source);
        if (!module) return std::unexpected(core::Error{module.error()});
        auto props = detail::read_properties(*module);
        *module = sol::table(); // release before the state closes
        return props;
    }();
    impl_->descriptions[path] = d;
    return d.properties;
}

} // namespace engine::script
