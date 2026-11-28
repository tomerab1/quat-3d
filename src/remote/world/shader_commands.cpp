// shader.list / get / template / create / update / delete — authored material
// shaders (ROADMAP A6), on top of rhi::MaterialShaderRegistry. create and
// update are deferred: the reply carries the compile outcome (with slangc's
// diagnostics, located by line and column) once the shader hot reload service
// has compiled the new source. Contract: docs/remote-control.md.

#include <memory>
#include <string>
#include <utility>
#include <vector>

#include <nlohmann/json.hpp>

#include "engine/rhi/material_shader_names.hpp"
#include "engine/rhi/material_shader_registry.hpp"
#include "engine/scene/components.hpp"
#include "engine/scene/scene.hpp"
#include "internal.hpp"

namespace engine::remote::world {

namespace {

using Registry = rhi::MaterialShaderRegistry;

[[nodiscard]] std::unexpected<RpcError> registry_error(const Registry::Error& e) {
    switch (e.kind) {
    case Registry::ErrorKind::invalid: return rpc_fail(rpc_code::invalid_params, e.message);
    case Registry::ErrorKind::not_found: return rpc_fail(rpc_code::not_found, e.message);
    case Registry::ErrorKind::unavailable: return rpc_fail(rpc_code::unavailable, e.message);
    case Registry::ErrorKind::exists:
    case Registry::ErrorKind::failed: break;
    }
    return rpc_fail(rpc_code::failed, e.message);
}

[[nodiscard]] Json diagnostic_json(const rhi::ShaderDiagnostic& d) {
    Json j{{"severity", d.severity}, {"message", d.message}};
    if (!d.file.empty()) j["file"] = d.file;
    if (d.line > 0) j["line"] = d.line;
    if (d.column > 0) j["column"] = d.column;
    if (d.code > 0) j["code"] = d.code;
    if (!d.source_line.empty()) j["source_line"] = d.source_line;
    return j;
}

[[nodiscard]] Json diagnostics_json(const std::vector<rhi::ShaderDiagnostic>& ds) {
    Json out = Json::array();
    for (const rhi::ShaderDiagnostic& d : ds) out.push_back(diagnostic_json(d));
    return out;
}

[[nodiscard]] std::size_t count_errors(const std::vector<rhi::ShaderDiagnostic>& ds) {
    std::size_t n = 0;
    for (const rhi::ShaderDiagnostic& d : ds) n += d.severity.find("error") != std::string::npos;
    return n;
}

[[nodiscard]] Json info_json(const Registry::Info& i) {
    return Json{{"name", i.name},
                {"builtin", i.builtin},
                {"path", i.path},
                {"state", i.state},
                {"reloads", i.reloads},
                {"errors", count_errors(i.diagnostics)}};
}

[[nodiscard]] std::expected<Registry*, RpcError> registry_of(const Context& ctx) {
    if (ctx.services.material_shaders == nullptr) return unavailable("material shaders");
    return ctx.services.material_shaders;
}

// Reads the required `name` (and, when `source` is non-null, the required
// `source`) parameters.
[[nodiscard]] std::expected<void, RpcError> read_name(const Json& params, std::string& name,
                                                      std::string* source) {
    core::JsonFields f(params, "");
    f.string("name", name, rhi::material_shaders::max_name_length);
    if (source != nullptr) f.string("source", *source);
    if (auto ok = finish(f); !ok) return ok;
    if (name.empty()) return bad_params("name: required (a material shader name)");
    if (source != nullptr && !f.has("source")) {
        return bad_params("source: required (the Slang module text)");
    }
    return {};
}

RpcResult shader_list(Context& ctx, const Json& params) {
    core::JsonFields f(params, "");
    if (auto ok = finish(f); !ok) return std::unexpected(ok.error());
    auto reg = registry_of(ctx);
    if (!reg) return std::unexpected(reg.error());
    Json shaders = Json::array();
    for (const Registry::Info& i : (*reg)->list()) shaders.push_back(info_json(i));
    return Json{{"shaders", std::move(shaders)}, {"can_compile", (*reg)->can_compile()}};
}

RpcResult shader_get(Context& ctx, const Json& params) {
    std::string name;
    if (auto ok = read_name(params, name, nullptr); !ok) return std::unexpected(ok.error());
    auto reg = registry_of(ctx);
    if (!reg) return std::unexpected(reg.error());
    auto info = (*reg)->info(name);
    if (!info) return registry_error(info.error());
    auto source = (*reg)->source(name);
    if (!source) return registry_error(source.error());
    Json out = info_json(*info);
    out["source"] = std::move(*source);
    out["diagnostics"] = diagnostics_json(info->diagnostics);
    out["log"] = info->message;
    return out;
}

RpcResult shader_template(Context& ctx, const Json& params) {
    core::JsonFields f(params, "");
    if (auto ok = finish(f); !ok) return std::unexpected(ok.error());
    auto reg = registry_of(ctx);
    if (!reg) return std::unexpected(reg.error());
    auto text = (*reg)->template_source();
    if (!text) return registry_error(text.error());
    return Json{{"source", std::move(*text)},
                {"save_as", "assets/shaders/materials/<name>.slang"}};
}

// Entities whose Primitive (or InstancedMesh primitive) selects `name`.
[[nodiscard]] std::size_t reset_references(entt::registry& r, const std::string& name) {
    std::size_t n = 0;
    for (auto [e, p] : r.view<scene::Primitive>().each()) {
        if (p.shader == name) {
            p.shader.clear();
            ++n;
        }
    }
    for (auto [e, m] : r.view<scene::InstancedMesh>().each()) {
        if (m.primitive.shader == name) {
            m.primitive.shader.clear();
            ++n;
        }
    }
    return n;
}

RpcResult shader_delete(Context& ctx, const Json& params) {
    std::string name;
    if (auto ok = read_name(params, name, nullptr); !ok) return std::unexpected(ok.error());
    auto reg = registry_of(ctx);
    if (!reg) return std::unexpected(reg.error());
    if (auto ok = (*reg)->remove(name); !ok) return registry_error(ok.error());
    // Entities fall back to the built-in material rather than keep drawing a
    // program whose source is gone.
    const std::size_t reset = reset_references(ctx.scene->registry(), name);
    return Json{{"deleted", name}, {"entities_reset", reset}};
}

void shader_write(const ContextPtr& ctx, const Json& params, Responder responder, bool create) {
    std::string name;
    std::string source;
    if (auto ok = read_name(params, name, &source); !ok) {
        responder.reply(std::unexpected(ok.error()));
        return;
    }
    auto reg = registry_of(*ctx);
    if (!reg) {
        responder.reply(std::unexpected(reg.error()));
        return;
    }
    // std::function needs a copyable callable; the Responder is move-only.
    auto shared = std::make_shared<Responder>(std::move(responder));
    Registry::Done done = [shared](std::expected<Registry::Outcome, Registry::Error> outcome) {
        if (!outcome) {
            shared->reply(registry_error(outcome.error()));
            return;
        }
        Json out{{"name", outcome->name},
                 {"path", outcome->path},
                 {"ok", outcome->ok},
                 {"errors", count_errors(outcome->diagnostics)},
                 {"diagnostics", diagnostics_json(outcome->diagnostics)},
                 {"log", outcome->log}};
        if (!outcome->ok) out["kept_previous"] = outcome->kept_previous;
        shared->reply(std::move(out));
    };
    if (create) {
        (*reg)->create(std::move(name), std::move(source), std::move(done));
    } else {
        (*reg)->update(std::move(name), std::move(source), std::move(done));
    }
}

} // namespace

void register_shader_commands(RpcServer& server, const ContextPtr& ctx) {
    server.add_method("shader.list",
                      "Material shaders (built-in + assets/shaders/materials) with compile state.",
                      [ctx](const Json& p) { return shader_list(*ctx, p); });
    server.add_method("shader.get", "One material shader: source, state and diagnostics.",
                      [ctx](const Json& p) { return shader_get(*ctx, p); });
    server.add_method("shader.template", "Starter material shader module text.",
                      [ctx](const Json& p) { return shader_template(*ctx, p); });
    server.add_method("shader.delete",
                      "Delete a material shader; entities using it fall back to the built-in.",
                      [ctx](const Json& p) { return shader_delete(*ctx, p); });
    server.add_deferred_method(
        "shader.create", "Add a material shader and compile it: {ok, diagnostics, log}.",
        [ctx](const Json& p, Responder r) { shader_write(ctx, p, std::move(r), true); });
    server.add_deferred_method(
        "shader.update",
        "Replace a material shader's source and recompile; a failed compile keeps the old one.",
        [ctx](const Json& p, Responder r) { shader_write(ctx, p, std::move(r), false); });
}

} // namespace engine::remote::world
