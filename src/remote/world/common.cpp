// Shared world-command helpers + registration entry point.

#include <algorithm>
#include <cstdint>
#include <limits>
#include <string>
#include <system_error>
#include <unordered_set>
#include <utility>
#include <vector>

#include <nlohmann/json.hpp>

#include "engine/scene/component_json.hpp"
#include "engine/scene/components.hpp"
#include "engine/scene/scene.hpp"
#include "internal.hpp"

namespace engine::remote {

namespace world {

std::unexpected<RpcError> unavailable(std::string_view what) {
    return rpc_fail(rpc_code::unavailable,
                    std::string(what) + " is not available in this engine build or state");
}

std::expected<void, RpcError> finish(const core::JsonFields& fields) {
    if (auto ok = fields.finish(); !ok) return bad_params(ok.error().message);
    return {};
}

Json entity_json(entt::entity e) { return Json(entt::to_integral(e)); }

std::expected<entt::entity, RpcError> resolve_entity(const entt::registry& registry,
                                                     const Json& ref, std::string_view field) {
    const std::string f(field);
    if (ref.is_string()) {
        const std::string& name = ref.get_ref<const std::string&>();
        std::vector<entt::entity> matches;
        for (auto [e, n] : registry.view<const scene::Name>().each()) {
            if (n.value == name) matches.push_back(e);
        }
        if (matches.empty()) {
            std::string hint;
            if (!name.empty() && name.front() == '@') {
                hint = " (\"@N\" back-references only work inside scene.batch)";
            }
            return rpc_fail(rpc_code::not_found, f + ": no entity named '" + name + "'" + hint);
        }
        if (matches.size() > 1) {
            std::sort(matches.begin(), matches.end());
            std::string ids;
            for (std::size_t i = 0; i < matches.size() && i < 8; ++i) {
                ids += (i == 0 ? "" : ", ") + std::to_string(entt::to_integral(matches[i]));
            }
            if (matches.size() > 8) ids += ", ...";
            return bad_params(f + ": " + std::to_string(matches.size()) + " entities are named '" +
                              name + "' (ids " + ids + "); use an id");
        }
        return matches.front();
    }
    std::int64_t id = 0;
    if (!core::JsonFields::read_int(ref, id) || id < 0 ||
        id > static_cast<std::int64_t>(std::numeric_limits<std::uint32_t>::max())) {
        return bad_params(f + ": expected an entity id (integer) or a unique entity name (string)");
    }
    const auto e = static_cast<entt::entity>(static_cast<std::uint32_t>(id));
    if (!registry.valid(e)) {
        return rpc_fail(rpc_code::not_found, f + ": no entity with id " + std::to_string(id));
    }
    return e;
}

std::expected<entt::entity, RpcError> resolve_parent(const entt::registry& registry,
                                                     const Json& ref, std::string_view field) {
    if (ref.is_null()) return entt::entity{entt::null};
    return resolve_entity(registry, ref, field);
}

std::expected<entt::entity, RpcError> required_entity(core::JsonFields& fields,
                                                      const entt::registry& registry,
                                                      std::string_view key) {
    const Json* ref = fields.raw(key);
    if (ref == nullptr || ref->is_null()) {
        return bad_params(std::string(key) + ": required (an entity id or unique name)");
    }
    return resolve_entity(registry, *ref, key);
}

entt::entity parent_of(const entt::registry& registry, entt::entity e) {
    const auto* p = registry.try_get<scene::Parent>(e);
    return p != nullptr && registry.valid(p->entity) ? p->entity : entt::entity{entt::null};
}

glm::mat4 world_matrix(const entt::registry& registry, entt::entity e) {
    glm::mat4 m(1.0F);
    std::size_t guard = 0;
    for (entt::entity a = e; a != entt::null && registry.valid(a) && guard < 100'000; ++guard) {
        if (const auto* t = registry.try_get<scene::Transform>(a)) m = t->local * m;
        a = parent_of(registry, a);
    }
    return m;
}

namespace {
void propagate(entt::registry& registry, entt::entity e, const glm::mat4& parent_world,
               std::size_t depth) {
    if (depth > 10'000) return; // corrupt hierarchy guard
    glm::mat4 world = parent_world;
    if (auto* t = registry.try_get<scene::Transform>(e)) {
        t->world = parent_world * t->local;
        world = t->world;
    }
    if (const auto* kids = registry.try_get<scene::Children>(e)) {
        const std::vector<entt::entity> copy = kids->entities;
        for (const entt::entity c : copy) {
            if (registry.valid(c)) propagate(registry, c, world, depth + 1);
        }
    }
}
} // namespace

void refresh_world(entt::registry& registry, entt::entity e) {
    if (!registry.valid(e)) return;
    const entt::entity p = parent_of(registry, e);
    propagate(registry, e, p == entt::null ? glm::mat4(1.0F) : world_matrix(registry, p), 0);
}

std::vector<entt::entity> subtree(const entt::registry& registry, entt::entity root) {
    std::vector<entt::entity> out;
    std::unordered_set<entt::entity> seen;
    std::vector<entt::entity> stack{root};
    while (!stack.empty()) {
        const entt::entity e = stack.back();
        stack.pop_back();
        if (!registry.valid(e) || !seen.insert(e).second) continue;
        out.push_back(e);
        if (const auto* kids = registry.try_get<scene::Children>(e)) {
            for (auto it = kids->entities.rbegin(); it != kids->entities.rend(); ++it) {
                stack.push_back(*it);
            }
        }
    }
    return out;
}

namespace {
// Codec errors start with the component name ("point_light.radius: ...");
// prefix them so the path names the request field exactly.
[[nodiscard]] std::string components_path(std::string_view key, const std::string& message) {
    if (message.starts_with(std::string(key) + ".") || message.starts_with(std::string(key) + ":")) {
        return "components." + message;
    }
    return "components: " + message;
}
} // namespace

std::expected<void, RpcError> apply_components_atomic(entt::registry& registry,
                                                      entt::entity target,
                                                      const Json& components) {
    if (!components.is_object()) {
        return bad_params("components: expected an object of {\"<component>\": {fields}}");
    }
    // Dry run on a scratch copy of the entity's current components, so merge
    // semantics and cross-field checks see the same base values.
    entt::registry scratch;
    const entt::entity s = scratch.create();
    scratch.emplace<scene::Transform>(s);
    if (registry.valid(target)) {
        if (registry.all_of<scene::Animator>(target)) scratch.emplace<scene::Animator>(s);
        scene::EncodeOptions options;
        options.transform_as_matrix = true;
        options.include_read_only = false;
        const Json current = scene::encode_components(registry, target, options);
        for (const auto& [key, value] : current.items()) {
            (void)scene::apply_component(scratch, s, key, value, /*strict=*/false);
        }
    }
    for (const auto& [key, value] : components.items()) {
        if (auto ok = scene::apply_component(scratch, s, key, value, /*strict=*/true); !ok) {
            return bad_params(components_path(key, ok.error().message));
        }
    }
    bool moved = false;
    for (const auto& [key, value] : components.items()) {
        if (auto ok = scene::apply_component(registry, target, key, value, /*strict=*/true); !ok) {
            return rpc_fail(rpc_code::failed, components_path(key, ok.error().message));
        }
        moved = moved || key == "transform";
    }
    if (moved) refresh_world(registry, target);
    return {};
}

std::expected<std::filesystem::path, RpcError> project_path(const Context& ctx,
                                                            core::JsonFields& fields,
                                                            std::string_view key,
                                                            std::string& relative) {
    std::string raw;
    fields.string(key, raw, 4096);
    if (!fields.ok()) return std::unexpected(finish(fields).error());
    if (!ctx.services.project_root) return unavailable("project file access");
    if (raw.empty()) {
        return bad_params(std::string(key) + ": required (a path relative to the project root)");
    }
    auto confined = confine_to_root(ctx.services.project_root(), raw);
    if (!confined) return bad_params(std::string(key) + ": " + confined.error().message);
    relative = std::filesystem::path(raw).lexically_normal().generic_string();
    return *confined;
}

} // namespace world

std::expected<std::filesystem::path, core::Error> confine_to_root(const std::filesystem::path& root,
                                                                  std::string_view relative) {
    namespace fs = std::filesystem;
    const auto fail = [](std::string what) { return std::unexpected(core::Error{std::move(what)}); };
    if (relative.empty()) return fail("expected a path relative to the project root");
    if (relative.find('\0') != std::string_view::npos) return fail("path contains a NUL byte");
    const fs::path p{std::string(relative)};
    if (p.is_absolute() || p.has_root_name() || p.has_root_directory()) {
        return fail("absolute paths are not allowed; give a path relative to the project root");
    }
    const fs::path norm = p.lexically_normal();
    if (norm.empty() || norm == ".") return fail("expected a file path, not the project root");
    if (*norm.begin() == "..") return fail("the path escapes the project root");

    // Symlinks inside the project could still point outside: compare the
    // resolved forms of the existing prefixes too.
    std::error_code ec;
    const fs::path canon_root = fs::weakly_canonical(root, ec);
    if (ec) return fail("cannot resolve the project root: " + ec.message());
    const fs::path canon = fs::weakly_canonical(root / norm, ec);
    if (ec) return fail("cannot resolve the path: " + ec.message());
    const fs::path rel = canon.lexically_relative(canon_root);
    if (rel.empty() || *rel.begin() == "..") return fail("the path escapes the project root");
    return root / norm;
}

void register_world_commands(RpcServer& server, scene::Scene& scene, WorldServices services) {
    auto ctx = std::make_shared<world::Context>();
    ctx->server = &server;
    ctx->scene = &scene;
    ctx->services = std::move(services);
    world::register_scene_commands(server, ctx);
    world::register_batch_command(server, ctx);
    world::register_environment_commands(server, ctx);
    world::register_camera_commands(server, ctx);
    world::register_app_commands(server, ctx);
    world::register_debug_commands(server, ctx);
    world::register_debug_query_commands(server, ctx);
    world::register_behavior_commands(server, ctx);
    world::register_behavior_edit_commands(server, ctx);
}

} // namespace engine::remote
