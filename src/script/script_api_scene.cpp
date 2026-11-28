// The `scene` table scripts use — see docs/scripting.md ("Scene").

#include <algorithm>
#include <filesystem>
#include <string>
#include <vector>

#include <entt/entity/registry.hpp>
#include <nlohmann/json.hpp>

#include "engine/scene/component_json.hpp"
#include "engine/scene/components.hpp"
#include "engine/scene/scene.hpp"
#include "lua_vm.hpp"
#include "script_api.hpp"

namespace engine::script::detail {

namespace {

namespace fs = std::filesystem;
constexpr std::size_t k_max_clone_entities = 4096;

[[nodiscard]] sol::object entity_object(lua_State* L, entt::entity e) {
    return sol::make_object(L, ScriptEntity{e});
}

// Applies the component fields of `spec` (every key but name / parent) in
// the codec's canonical order. Strict: unknown components or fields fail.
[[nodiscard]] Result<void> apply_spec(entt::registry& r, entt::entity e, const nlohmann::json& spec) {
    for (const auto& [key, value] : spec.items()) {
        if (key == "name" || key == "parent") continue;
        if (scene::find_component_info(key) == nullptr) {
            return fail("unknown component '" + key + "' (spec keys are name, parent and component names)");
        }
    }
    for (const scene::ComponentInfo& info : scene::component_infos()) {
        const auto it = spec.find(std::string(info.name));
        if (it == spec.end()) continue;
        if (auto ok = scene::apply_component(r, e, info.name, *it, true); !ok) {
            return fail(ok.error().message);
        }
    }
    return {};
}

// Copies `source` (and, recursively, its children) onto new entities.
[[nodiscard]] entt::entity clone_tree(scene::Scene& scene, entt::entity source, entt::entity parent,
                                      std::size_t& budget) {
    entt::registry& r = scene.registry();
    if (budget == 0) return entt::null;
    --budget;
    const auto* name = r.try_get<scene::Name>(source);
    const entt::entity copy = scene.create_entity(name != nullptr ? name->value : std::string());
    const nlohmann::json components = scene::encode_components(r, source, {.include_read_only = false});
    for (const auto& [key, value] : components.items()) {
        (void)scene::apply_component(r, copy, key, value, false);
    }
    r.get<scene::Transform>(copy).local = r.get<scene::Transform>(source).local;
    // Model data the codec does not write (glTF meshes, skins, animation).
    if (const auto* mr = r.try_get<scene::MeshRenderer>(source)) {
        const scene::MeshRenderer value = *mr;
        r.emplace_or_replace<scene::MeshRenderer>(copy, value);
    }
    if (const auto* ms = r.try_get<scene::MeshSource>(source)) {
        const scene::MeshSource value = *ms;
        r.emplace_or_replace<scene::MeshSource>(copy, value);
    }
    if (const auto* sk = r.try_get<scene::SkinnedMesh>(source)) {
        const scene::SkinnedMesh value = *sk;
        r.emplace_or_replace<scene::SkinnedMesh>(copy, value);
    }
    if (const auto* an = r.try_get<scene::Animator>(source)) {
        const scene::Animator value = *an;
        r.emplace_or_replace<scene::Animator>(copy, value);
    }
    if (auto* net = r.try_get<scene::NetReplicated>(copy)) net->net_id = 0; // reassigned by the session
    if (parent != entt::null) (void)scene.set_parent(copy, parent);
    if (const auto* ch = r.try_get<scene::Children>(source)) {
        const std::vector<entt::entity> children = ch->entities; // copy: creation may reallocate
        for (const entt::entity child : children) {
            if (r.valid(child)) (void)clone_tree(scene, child, copy, budget);
        }
    }
    return copy;
}

[[nodiscard]] Result<fs::path> model_path(const ApiContext& ctx, const std::string& relative) {
    const fs::path p(relative);
    if (relative.empty() || p.is_absolute() || p.has_root_name() || p.has_root_directory() ||
        relative.find('\0') != std::string::npos) {
        return fail("expected a model path relative to the project root, e.g. \"assets/meshes/crate.glb\"");
    }
    const fs::path norm = p.lexically_normal();
    if (norm.empty() || *norm.begin() == "..") return fail("the model path escapes the project root");
    const std::string ext = norm.extension().string();
    if (ext != ".gltf" && ext != ".glb") return fail("models are .gltf or .glb files");
    std::error_code ec;
    const fs::path root = fs::weakly_canonical(ctx.services.project_root, ec);
    const fs::path canon = fs::weakly_canonical(ctx.services.project_root / norm, ec);
    const fs::path rel = canon.lexically_relative(root);
    if (ec || rel.empty() || *rel.begin() == "..") return fail("the model path escapes the project root");
    return ctx.services.project_root / norm;
}

} // namespace

void register_scene(LuaVm& vm, ApiContext& c) {
    sol::state_view lua = vm.lua();
    lua_State* L = vm.state();
    ApiContext* ctx = &c;
    sol::table t(L, sol::create);

    t.set("find", bind(L, [ctx](const std::string& name, sol::this_state s) -> Result<sol::object> {
              auto r = scene_registry(*ctx);
              if (!r.value) return std::unexpected(r.value.error());
              for (const auto [e, n] : (*r.value)->view<scene::Name>().each()) {
                  if (n.value == name) return entity_object(s.lua_state(), e);
              }
              return sol::make_object(s.lua_state(), sol::lua_nil);
          }));
    t.set("find_all", bind(L, [ctx](const std::string& name, sol::this_state s) -> Result<sol::object> {
              auto r = scene_registry(*ctx);
              if (!r.value) return std::unexpected(r.value.error());
              sol::table out(s.lua_state(), sol::create);
              int i = 1;
              for (const auto [e, n] : (*r.value)->view<scene::Name>().each()) {
                  if (n.value == name) out[i++] = ScriptEntity{e};
              }
              return sol::object(out);
          }));
    t.set("with_component", bind(L, [ctx](const std::string& component,
                                          sol::this_state s) -> Result<sol::object> {
              auto r = scene_registry(*ctx);
              if (!r.value) return std::unexpected(r.value.error());
              if (scene::find_component_info(component) == nullptr) {
                  return fail("unknown component '" + component + "'");
              }
              sol::table out(s.lua_state(), sol::create);
              int i = 1;
              for (const entt::entity e : (*r.value)->view<scene::Transform>()) {
                  const auto names = scene::component_names(**r.value, e);
                  if (std::ranges::find(names, component) != names.end()) out[i++] = ScriptEntity{e};
              }
              return sol::object(out);
          }));
    t.set("spawn", bind(L, [ctx](sol::optional<sol::table> spec) -> Result<ScriptEntity> {
              auto r = scene_registry(*ctx);
              if (!r.value) return std::unexpected(r.value.error());
              nlohmann::json json = nlohmann::json::object();
              if (spec) {
                  auto converted = lua_to_json(*spec, true);
                  if (!converted.value) return fail("spawn: " + converted.value.error().message);
                  json = std::move(*converted.value);
              }
              std::string name;
              if (const auto it = json.find("name"); it != json.end()) {
                  if (!it->is_string()) return fail("spawn: name must be a string");
                  name = it->get<std::string>();
              }
              entt::entity parent = entt::null;
              if (spec) {
                  const sol::object p = spec->raw_get<sol::object>("parent");
                  if (p.is<ScriptEntity>()) {
                      parent = p.as<ScriptEntity>().id;
                      if (!(*r.value)->valid(parent)) return fail("spawn: the parent no longer exists");
                  } else if (p.valid() && p.get_type() != sol::type::lua_nil) {
                      return fail("spawn: parent must be an Entity");
                  }
              }
              const entt::entity e = ctx->scene->create_entity(name);
              if (parent != entt::null) (void)ctx->scene->set_parent(e, parent);
              if (auto ok = apply_spec(**r.value, e, json); !ok.value) {
                  (*r.value)->destroy(e);
                  if (parent != entt::null) {
                      if (auto* ch = (*r.value)->try_get<scene::Children>(parent)) {
                          std::erase(ch->entities, e);
                      }
                  }
                  return fail("spawn: " + ok.value.error().message);
              }
              refresh_world(**r.value, e);
              return ScriptEntity{e};
          }));
    t.set("clone", bind(L, [ctx](const ScriptEntity& source) -> Result<ScriptEntity> {
              auto r = live_entity(*ctx, source);
              if (!r.value) return std::unexpected(r.value.error());
              std::size_t budget = k_max_clone_entities;
              entt::entity parent = entt::null;
              if (const auto* p = (*r.value)->try_get<scene::Parent>(source.id)) parent = p->entity;
              const entt::entity copy = clone_tree(*ctx->scene, source.id, parent, budget);
              refresh_world(**r.value, copy);
              return ScriptEntity{copy};
          }));
    t.set("destroy", bind(L, [ctx](const ScriptEntity& e) -> Result<void> {
              auto r = live_entity(*ctx, e);
              if (!r.value) return std::unexpected(r.value.error());
              ctx->pending_destroy.push_back(e.id);
              return {};
          }));
    t.set("spawn_model", bind(L, [ctx](const std::string& path,
                                       sol::optional<sol::table> spec) -> Result<ScriptEntity> {
              auto r = scene_registry(*ctx);
              if (!r.value) return std::unexpected(r.value.error());
              if (!ctx->services.spawn_model) return fail("spawn_model: models cannot be loaded here");
              auto file = model_path(*ctx, path);
              if (!file.value) return fail("spawn_model: " + file.value.error().message);
              nlohmann::json json = nlohmann::json::object();
              if (spec) {
                  auto converted = lua_to_json(*spec, true);
                  if (!converted.value) return fail("spawn_model: " + converted.value.error().message);
                  json = std::move(*converted.value);
              }
              auto roots = ctx->services.spawn_model(*file.value);
              if (!roots) return fail("spawn_model: " + roots.error().message);
              if (roots->empty()) return fail("spawn_model: the model has no nodes");
              entt::entity root = roots->front();
              if (roots->size() > 1) { // group several roots under one entity
                  root = ctx->scene->create_entity(file.value->stem().string());
                  for (const entt::entity e : *roots) (void)ctx->scene->set_parent(e, root);
              }
              if (const auto it = json.find("name"); it != json.end() && it->is_string()) {
                  (*r.value)->emplace_or_replace<scene::Name>(root, it->get<std::string>());
              }
              if (auto ok = apply_spec(**r.value, root, json); !ok.value) {
                  return fail("spawn_model: " + ok.value.error().message);
              }
              refresh_world(**r.value, root);
              return ScriptEntity{root};
          }));
    t.set("count", bind(L, [ctx]() -> Result<std::size_t> {
              auto r = scene_registry(*ctx);
              if (!r.value) return std::unexpected(r.value.error());
              return (*r.value)->view<scene::Transform>().size();
          }));
    vm.expose("scene", t);
}

} // namespace engine::script::detail
