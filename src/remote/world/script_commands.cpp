// script.* — gameplay script files under assets/scripts/ (ROADMAP G7):
// list, read, create, update, delete and compile-check. Paths are confined to
// the scripts directory (scene::validate_script_path plus a symlink check),
// sources are size-limited and compiled before they are written, so a
// syntax error is reported with its line and never reaches the disk.

#include <fstream>
#include <random>
#include <string>
#include <system_error>

#ifndef _WIN32
#include <fcntl.h>
#include <unistd.h>
#endif

#include <entt/entity/registry.hpp>
#include <nlohmann/json.hpp>

#include "engine/scene/components.hpp"
#include "engine/scene/scene.hpp"
#include "engine/scene/script_component.hpp"
#include "engine/script/script_runtime.hpp"
#include "engine/script/script_source.hpp"
#include "internal.hpp"

namespace engine::remote::world {

namespace {

namespace fs = std::filesystem;

[[nodiscard]] std::expected<fs::path, RpcError> scripts_root(const Context& ctx) {
    if (!ctx.services.project_root) return unavailable("project file access");
    return ctx.services.project_root() / std::string(scene::k_scripts_dir);
}

[[nodiscard]] script::ScriptRuntime* runtime(const Context& ctx) {
    return ctx.services.scripts ? ctx.services.scripts() : nullptr;
}

// The confined file of the `path` parameter.
[[nodiscard]] std::expected<fs::path, RpcError> script_path(const Context& ctx, core::JsonFields& f,
                                                            std::string& path) {
    f.string("path", path, scene::k_max_script_path);
    if (auto ok = finish(f); !ok) return std::unexpected(ok.error());
    if (path.empty()) return bad_params("path: required, e.g. \"coin.lua\" (relative to assets/scripts/)");
    auto root = scripts_root(ctx);
    if (!root) return std::unexpected(root.error());
    auto file = script::script_file(*root, path);
    if (!file) return bad_params("path: " + file.error().message);
    return *file;
}

[[nodiscard]] Json status_json(const script::ScriptStatus* s) {
    if (s == nullptr) return nullptr;
    Json out = {{"state", s->state == script::ScriptStatus::State::ok ? "ok" : "error"},
                {"instances", s->instances},
                {"failed", s->failed},
                {"reloads", s->reloads}};
    if (!s->message.empty()) out["message"] = s->message;
    return out;
}

[[nodiscard]] const script::ScriptStatus* find_status(const Context& ctx, const std::string& path) {
    const script::ScriptRuntime* rt = runtime(ctx);
    if (rt == nullptr) return nullptr;
    for (const script::ScriptStatus& s : rt->statuses()) {
        if (s.path == path) return &s;
    }
    return nullptr;
}

[[nodiscard]] std::size_t users(const Context& ctx, const std::string& path) {
    std::size_t n = 0;
    for (const auto [e, sc] : ctx.scene->registry().view<scene::Script>().each()) {
        n += sc.path == path ? 1U : 0U;
    }
    return n;
}

// Compiles `source`; a syntax error is invalid_params carrying {path, line}.
[[nodiscard]] std::expected<void, RpcError> compile(const std::string& path, const std::string& source) {
    if (source.size() > script::k_max_script_bytes) {
        return bad_params("source: larger than " + std::to_string(script::k_max_script_bytes) + " bytes");
    }
    if (source.find('\0') != std::string::npos) return bad_params("source: contains a NUL byte");
    auto ok = script::check_syntax(source, path);
    if (ok) return {};
    RpcError e{rpc_code::invalid_params,
               "syntax error" +
                   (ok.error().line > 0 ? " at line " + std::to_string(ok.error().line) : std::string()) +
                   ": " + ok.error().message,
               Json{{"path", path}, {"line", ok.error().line}}};
    return std::unexpected(std::move(e));
}

// Creates `tmp` exclusively (never through an existing file or symlink) and
// writes `source` to it.
[[nodiscard]] bool write_new_file(const fs::path& tmp, const std::string& source) {
#ifndef _WIN32
    const int fd = ::open(tmp.c_str(), O_WRONLY | O_CREAT | O_EXCL | O_NOFOLLOW | O_CLOEXEC, 0644);
    if (fd < 0) return false;
    std::size_t done = 0;
    while (done < source.size()) {
        const ::ssize_t n = ::write(fd, source.data() + done, source.size() - done);
        if (n <= 0) break;
        done += static_cast<std::size_t>(n);
    }
    return ::close(fd) == 0 && done == source.size();
#else
    std::error_code ec;
    if (fs::symlink_status(tmp, ec).type() != fs::file_type::not_found) return false;
    std::ofstream out(tmp, std::ios::binary | std::ios::trunc);
    out.write(source.data(), static_cast<std::streamsize>(source.size()));
    return static_cast<bool>(out);
#endif
}

// Writes through a uniquely named temporary file next to the script and a
// rename, so readers (the hot-reload watcher) never see a half-written
// script. The temporary is created exclusively without following links, and
// the destination is confined again right before the rename (a directory
// swapped for a symlink meanwhile is refused).
[[nodiscard]] std::expected<void, RpcError> write_file(const fs::path& root, const std::string& relative,
                                                       const fs::path& file, const std::string& source) {
    std::error_code ec;
    fs::create_directories(file.parent_path(), ec);
    if (ec) return rpc_fail(rpc_code::failed, "cannot create " + file.parent_path().filename().string() + ": " + ec.message());
    if (!script::script_file(root, relative)) return bad_params("path: resolves outside assets/scripts/");
    std::random_device rd;
    const fs::path tmp = file.parent_path() /
                         ("." + file.filename().string() + "." + std::to_string(rd()) + ".tmp");
    if (!write_new_file(tmp, source)) {
        fs::remove(tmp, ec);
        return rpc_fail(rpc_code::failed, "cannot write the script file");
    }
    if (!script::script_file(root, relative)) {
        fs::remove(tmp, ec);
        return bad_params("path: resolves outside assets/scripts/");
    }
    fs::rename(tmp, file, ec);
    if (ec) {
        fs::remove(tmp, ec);
        return rpc_fail(rpc_code::failed, "cannot replace the script file");
    }
    return {};
}

RpcResult script_list(Context& ctx, const Json& params) {
    core::JsonFields f(params, "");
    if (auto ok = finish(f); !ok) return std::unexpected(ok.error());
    auto root = scripts_root(ctx);
    if (!root) return std::unexpected(root.error());
    Json scripts = Json::array();
    for (const std::string& path : script::list_scripts(*root)) {
        std::error_code ec;
        const auto bytes = fs::file_size(*root / path, ec);
        Json entry = {{"path", path}, {"bytes", ec ? 0 : bytes}, {"entities", users(ctx, path)}};
        if (const script::ScriptStatus* s = find_status(ctx, path)) entry["status"] = status_json(s);
        scripts.push_back(std::move(entry));
    }
    const script::ScriptRuntime* rt = runtime(ctx);
    return Json{{"scripts", std::move(scripts)}, {"running", rt != nullptr && rt->running()}};
}

RpcResult script_get(Context& ctx, const Json& params) {
    core::JsonFields f(params, "");
    std::string path;
    auto file = script_path(ctx, f, path);
    if (!file) return std::unexpected(file.error());
    std::error_code ec;
    if (!fs::is_regular_file(*file, ec)) return rpc_fail(rpc_code::not_found, "no script assets/scripts/" + path);
    auto source = script::read_script(*file);
    if (!source) return rpc_fail(rpc_code::failed, source.error().message);
    Json out = {{"path", path}, {"source", std::move(*source)}, {"entities", users(ctx, path)}};
    out["status"] = status_json(find_status(ctx, path));
    return out;
}

RpcResult script_write(Context& ctx, const Json& params, bool create) {
    core::JsonFields f(params, "");
    std::string source;
    f.string("source", source, script::k_max_script_bytes);
    if (f.ok() && !f.has("source")) f.fail("source", "required: the Lua source text");
    std::string path;
    auto file = script_path(ctx, f, path);
    if (!file) return std::unexpected(file.error());
    std::error_code ec;
    const bool exists = fs::exists(*file, ec);
    if (create && exists) {
        return rpc_fail(rpc_code::failed, "assets/scripts/" + path + " already exists (use script.update)");
    }
    if (!create && !exists) {
        return rpc_fail(rpc_code::not_found, "no script assets/scripts/" + path + " (use script.create)");
    }
    if (auto ok = compile(path, source); !ok) return std::unexpected(ok.error());
    auto root = scripts_root(ctx);
    if (!root) return std::unexpected(root.error());
    if (auto ok = write_file(*root, path, *file, source); !ok) return std::unexpected(ok.error());
    script::ScriptRuntime* rt = runtime(ctx);
    if (rt != nullptr) rt->reload(path); // restarts running instances (hot reload)
    Json out = {{"path", path}, {"bytes", source.size()}, {"entities", users(ctx, path)}};
    if (rt != nullptr) {
        out["running"] = rt->running();
        out["status"] = status_json(find_status(ctx, path));
    }
    return out;
}

RpcResult script_delete(Context& ctx, const Json& params) {
    core::JsonFields f(params, "");
    std::string path;
    auto file = script_path(ctx, f, path);
    if (!file) return std::unexpected(file.error());
    std::error_code ec;
    if (!fs::is_regular_file(*file, ec)) return rpc_fail(rpc_code::not_found, "no script assets/scripts/" + path);
    if (!fs::remove(*file, ec) || ec) return rpc_fail(rpc_code::failed, "cannot delete the script file");
    return Json{{"path", path}, {"deleted", true}, {"entities_still_using", users(ctx, path)}};
}

RpcResult script_check(Context&, const Json& params) {
    core::JsonFields f(params, "");
    std::string source;
    std::string path = "check.lua";
    f.string("source", source, script::k_max_script_bytes).string("path", path, scene::k_max_script_path);
    if (f.ok() && !f.has("source")) f.fail("source", "required: the Lua source text");
    if (auto ok = finish(f); !ok) return std::unexpected(ok.error());
    if (auto ok = compile(path, source); !ok) {
        return Json{{"ok", false}, {"line", ok.error().data["line"]}, {"message", ok.error().message}};
    }
    return Json{{"ok", true}};
}

} // namespace

void register_script_commands(RpcServer& server, const ContextPtr& ctx) {
    server.add_method("script.list",
                      "Lua scripts under assets/scripts/: {} -> {scripts:[{path, bytes, entities, "
                      "status?}], running}.",
                      [ctx](const Json& p) { return script_list(*ctx, p); });
    server.add_method("script.get", "Read a script: {path} -> {path, source, entities, status}.",
                      [ctx](const Json& p) { return script_get(*ctx, p); });
    server.add_method("script.create",
                      "Create a script (compiled first; syntax errors name the line): {path, "
                      "source} -> {path, bytes, entities}.",
                      [ctx](const Json& p) { return script_write(*ctx, p, true); });
    server.add_method("script.update",
                      "Replace a script and hot-reload it: {path, source} -> {path, bytes, "
                      "entities, running?, status?}.",
                      [ctx](const Json& p) { return script_write(*ctx, p, false); });
    server.add_method("script.delete", "Delete a script: {path} -> {path, deleted, entities_still_using}.",
                      [ctx](const Json& p) { return script_delete(*ctx, p); });
    server.add_method("script.check",
                      "Compile Lua without saving it: {source, path?} -> {ok, line?, message?}.",
                      [ctx](const Json& p) { return script_check(*ctx, p); });
}

} // namespace engine::remote::world
