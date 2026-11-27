// scene.* — entity listing, inspection and editing (ROADMAP A2).

#include <algorithm>
#include <string>
#include <vector>

#include <glm/gtc/matrix_inverse.hpp>
#include <nlohmann/json.hpp>

#include "engine/scene/component_json.hpp"
#include "engine/scene/components.hpp"
#include "engine/scene/scene.hpp"
#include "internal.hpp"

namespace engine::remote::world {

namespace {

constexpr std::size_t k_max_name = 256;

[[nodiscard]] std::string name_of(const entt::registry& r, entt::entity e) {
    const auto* n = r.try_get<scene::Name>(e);
    return n != nullptr ? n->value : std::string();
}

[[nodiscard]] Json parent_json(const entt::registry& r, entt::entity e) {
    const entt::entity p = parent_of(r, e);
    return p == entt::null ? Json(nullptr) : entity_json(p);
}

[[nodiscard]] Json names_json(const std::vector<std::string_view>& names) {
    Json out = Json::array();
    for (std::string_view n : names) out.push_back(std::string(n));
    return out;
}

// Destroys `root` and its descendants after detaching it from its parent.
std::size_t destroy_subtree(scene::Scene& scene, entt::entity root) {
    entt::registry& r = scene.registry();
    const std::vector<entt::entity> doomed = subtree(r, root);
    (void)scene.set_parent(root, entt::null);
    for (auto it = doomed.rbegin(); it != doomed.rend(); ++it) r.destroy(*it);
    return doomed.size();
}

RpcResult scene_components(const Json&) {
    Json out = Json::array();
    for (const scene::ComponentInfo& c : scene::component_infos()) {
        out.push_back({{"name", std::string(c.name)},
                       {"summary", std::string(c.summary)},
                       {"addable", c.addable},
                       {"writable", c.writable}});
    }
    return out;
}

RpcResult scene_list(Context& ctx, const Json& params) {
    const entt::registry& r = ctx.scene->registry();
    core::JsonFields f(params, "");
    std::string name_contains;
    std::string component;
    int limit = 100;
    int offset = 0;
    f.string("name_contains", name_contains, k_max_name).string("component", component, 64);
    f.integer("limit", limit, 1, 10'000).integer("offset", offset, 0, 100'000'000);
    if (auto ok = finish(f); !ok) return std::unexpected(ok.error());
    if (!component.empty() && scene::find_component_info(component) == nullptr) {
        std::string known;
        for (const auto& c : scene::component_infos()) {
            known += (known.empty() ? "" : ", ") + std::string(c.name);
        }
        return bad_params("component: unknown component '" + component + "' (known: " + known + ")");
    }

    std::vector<entt::entity> matches;
    for (const entt::entity e : r.view<const scene::Transform>()) {
        if (!name_contains.empty() && name_of(r, e).find(name_contains) == std::string::npos) {
            continue;
        }
        if (!component.empty()) {
            const auto names = scene::component_names(r, e);
            if (std::find(names.begin(), names.end(), component) == names.end()) continue;
        }
        matches.push_back(e);
    }
    std::sort(matches.begin(), matches.end());

    Json entities = Json::array();
    for (std::size_t i = static_cast<std::size_t>(offset);
         i < matches.size() && entities.size() < static_cast<std::size_t>(limit); ++i) {
        const entt::entity e = matches[i];
        entities.push_back({{"id", entity_json(e)},
                            {"name", name_of(r, e)},
                            {"parent", parent_json(r, e)},
                            {"components", names_json(scene::component_names(r, e))},
                            {"world_position", vec3_json(glm::vec3(world_matrix(r, e)[3]))},
                            {"world_bounds", bounds_json(subtree_world_bounds(ctx, e, true))}});
    }
    return Json{{"total", matches.size()}, {"entities", std::move(entities)}};
}

RpcResult scene_get(Context& ctx, const Json& params) {
    const entt::registry& r = ctx.scene->registry();
    core::JsonFields f(params, "");
    auto e = required_entity(f, r, "entity");
    if (auto ok = finish(f); !ok) return std::unexpected(ok.error());
    if (!e) return std::unexpected(e.error());

    Json children = Json::array();
    if (const auto* kids = r.try_get<scene::Children>(*e)) {
        for (const entt::entity c : kids->entities) {
            if (r.valid(c)) children.push_back(entity_json(c));
        }
    }
    return Json{{"id", entity_json(*e)},
                {"name", name_of(r, *e)},
                {"parent", parent_json(r, *e)},
                {"children", std::move(children)},
                {"world_bounds", bounds_json(subtree_world_bounds(ctx, *e, true))},
                {"local_bounds", bounds_json(subtree_local_bounds(ctx, *e))},
                {"components", scene::encode_components(r, *e)}};
}

RpcResult scene_create(Context& ctx, const Json& params) {
    scene::Scene& sc = *ctx.scene;
    entt::registry& r = sc.registry();
    core::JsonFields f(params, "");
    std::string name;
    f.string("name", name, k_max_name);
    const Json* parent_ref = f.raw("parent");
    const Json* components = f.raw("components");
    if (auto ok = finish(f); !ok) return std::unexpected(ok.error());

    entt::entity parent = entt::null;
    if (parent_ref != nullptr) {
        auto p = resolve_parent(r, *parent_ref, "parent");
        if (!p) return std::unexpected(p.error());
        parent = *p;
    }
    // apply_components_atomic validates on a scratch entity first; a failed
    // create destroys the new entity, leaving no trace.
    const entt::entity e = sc.create_entity(name);
    if (components != nullptr && !components->is_null()) {
        if (auto ok = apply_components_atomic(r, e, *components); !ok) {
            r.destroy(e);
            return std::unexpected(ok.error());
        }
    }
    if (parent != entt::null && !sc.set_parent(e, parent)) {
        r.destroy(e);
        return rpc_fail(rpc_code::failed, "parent: cannot parent under that entity");
    }
    refresh_world(r, e);
    return Json{{"id", entity_json(e)}, {"name", name}};
}

RpcResult scene_update(Context& ctx, const Json& params) {
    entt::registry& r = ctx.scene->registry();
    core::JsonFields f(params, "");
    auto e = required_entity(f, r, "entity");
    std::string name;
    const bool rename = f.has("name");
    f.string("name", name, k_max_name);
    const Json* components = f.raw("components");
    if (auto ok = finish(f); !ok) return std::unexpected(ok.error());
    if (!e) return std::unexpected(e.error());

    if (components != nullptr && !components->is_null()) {
        if (auto ok = apply_components_atomic(r, *e, *components); !ok) {
            return std::unexpected(ok.error());
        }
    }
    if (rename) r.emplace_or_replace<scene::Name>(*e, name);
    return Json{{"id", entity_json(*e)}};
}

RpcResult scene_remove_components(Context& ctx, const Json& params) {
    entt::registry& r = ctx.scene->registry();
    core::JsonFields f(params, "");
    auto e = required_entity(f, r, "entity");
    const Json* list = f.raw("components");
    if (auto ok = finish(f); !ok) return std::unexpected(ok.error());
    if (!e) return std::unexpected(e.error());
    if (list == nullptr || !list->is_array()) {
        return bad_params("components: expected an array of component names");
    }
    for (const Json& n : *list) {
        if (!n.is_string() || scene::find_component_info(n.get_ref<const std::string&>()) == nullptr) {
            return bad_params("components: unknown component " + n.dump(-1, ' ', false,
                                                                         Json::error_handler_t::replace));
        }
        if (n == "transform") return bad_params("components: transform cannot be removed");
    }
    Json removed = Json::array();
    for (const Json& n : *list) {
        if (scene::remove_component(r, *e, n.get_ref<const std::string&>())) removed.push_back(n);
    }
    return Json{{"removed", std::move(removed)}};
}

RpcResult scene_destroy(Context& ctx, const Json& params) {
    core::JsonFields f(params, "");
    auto e = required_entity(f, ctx.scene->registry(), "entity");
    if (auto ok = finish(f); !ok) return std::unexpected(ok.error());
    if (!e) return std::unexpected(e.error());
    return Json{{"destroyed", destroy_subtree(*ctx.scene, *e)}};
}

RpcResult scene_set_parent(Context& ctx, const Json& params) {
    entt::registry& r = ctx.scene->registry();
    core::JsonFields f(params, "");
    auto e = required_entity(f, r, "entity");
    const Json* parent_ref = f.raw("parent");
    bool keep_world = true;
    f.boolean("keep_world", keep_world);
    if (auto ok = finish(f); !ok) return std::unexpected(ok.error());
    if (!e) return std::unexpected(e.error());
    if (parent_ref == nullptr) {
        return bad_params("parent: required (an entity id or unique name, or null for the root)");
    }
    auto parent = resolve_parent(r, *parent_ref, "parent");
    if (!parent) return std::unexpected(parent.error());

    const glm::mat4 world = world_matrix(r, *e);
    if (!ctx.scene->set_parent(*e, *parent)) {
        return rpc_fail(rpc_code::failed,
                        *parent == *e ? "parent: an entity cannot be its own parent"
                                      : "parent: refused — the new parent is a descendant of the "
                                        "entity (that would create a cycle)");
    }
    if (keep_world) {
        if (auto* t = r.try_get<scene::Transform>(*e)) {
            const glm::mat4 parent_world =
                *parent == entt::null ? glm::mat4(1.0F) : world_matrix(r, *parent);
            t->local = glm::inverse(parent_world) * world;
        }
    }
    refresh_world(r, *e);
    return Json{{"id", entity_json(*e)}};
}

RpcResult scene_clear(Context& ctx, const Json& params) {
    entt::registry& r = ctx.scene->registry();
    core::JsonFields f(params, "");
    bool keep_lights = true;
    f.boolean("keep_lights", keep_lights);
    if (auto ok = finish(f); !ok) return std::unexpected(ok.error());

    std::vector<entt::entity> keep;
    std::vector<entt::entity> doomed;
    for (const entt::entity e : r.view<const scene::Transform>()) {
        const bool light = r.any_of<scene::DirectionalLight, scene::PointLight>(e);
        (keep_lights && light ? keep : doomed).push_back(e);
    }
    // Kept lights leave the hierarchy (keeping their world placement) so no
    // destroyed ancestor dangles above them.
    for (const entt::entity e : keep) {
        const glm::mat4 world = world_matrix(r, e);
        if (ctx.scene->set_parent(e, entt::null)) r.get<scene::Transform>(e).local = world;
        r.remove<scene::Children>(e);
        refresh_world(r, e);
    }
    for (const entt::entity e : doomed) {
        if (r.valid(e)) r.destroy(e);
    }
    return Json{{"destroyed", doomed.size()}};
}

} // namespace

void register_scene_commands(RpcServer& server, const ContextPtr& ctx) {
    server.add_method("scene.components",
                      "Component types usable in scene.create/update: [{name, summary, addable, "
                      "writable}].",
                      scene_components);
    server.add_method("scene.list",
                      "List entities: {name_contains?, component?, limit?=100, offset?=0} -> "
                      "{total, entities:[{id, name, parent, components, world_position}]}.",
                      [ctx](const Json& p) { return scene_list(*ctx, p); });
    server.add_method("scene.get",
                      "One entity with every component: {entity} -> {id, name, parent, "
                      "children, components}.",
                      [ctx](const Json& p) { return scene_get(*ctx, p); });
    server.add_method("scene.create",
                      "Create an entity: {name?, parent?, components?} -> {id, name}. Atomic: a "
                      "bad component field creates nothing.",
                      [ctx](const Json& p) { return scene_create(*ctx, p); });
    server.add_method("scene.update",
                      "Merge-update an entity: {entity, name?, components?} -> {id}. Omitted "
                      "fields keep their values.",
                      [ctx](const Json& p) { return scene_update(*ctx, p); });
    server.add_method("scene.remove_components",
                      "Remove components: {entity, components:[names]} -> {removed:[names]}.",
                      [ctx](const Json& p) { return scene_remove_components(*ctx, p); });
    server.add_method("scene.destroy",
                      "Destroy an entity and its descendants: {entity} -> {destroyed: n}.",
                      [ctx](const Json& p) { return scene_destroy(*ctx, p); });
    server.add_method("scene.set_parent",
                      "Re-parent: {entity, parent (null = root), keep_world?=true} -> {id}. "
                      "Cycles are refused.",
                      [ctx](const Json& p) { return scene_set_parent(*ctx, p); });
    server.add_method("scene.clear",
                      "Destroy every entity: {keep_lights?=true} -> {destroyed: n}.",
                      [ctx](const Json& p) { return scene_clear(*ctx, p); });
}

} // namespace engine::remote::world
