// asset.*, sim.*, scene.save/load and nav.bake — the commands that reach the
// running application through WorldServices.

#include <algorithm>
#include <cctype>
#include <fstream>
#include <iterator>
#include <string>
#include <system_error>
#include <vector>

#include <nlohmann/json.hpp>

#include "engine/scene/components.hpp"
#include "engine/scene/scene.hpp"
#include "internal.hpp"

namespace engine::remote::world {

namespace {

namespace fs = std::filesystem;

constexpr std::size_t k_max_models = 10'000;
constexpr std::uintmax_t k_max_scene_bytes = 256ULL << 20;

[[nodiscard]] std::string lower_extension(const fs::path& p) {
    std::string ext = p.extension().string();
    std::transform(ext.begin(), ext.end(), ext.begin(),
                   [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    return ext;
}

[[nodiscard]] bool is_model(const fs::path& p) {
    const std::string ext = lower_extension(p);
    return ext == ".gltf" || ext == ".glb";
}

// An existing file may only be overwritten by scene.save when it already is
// a scene file — so a bad path cannot clobber the project's other JSON.
[[nodiscard]] bool looks_like_scene_file(const fs::path& path) {
    std::error_code ec;
    const std::uintmax_t size = fs::file_size(path, ec);
    if (ec || size > k_max_scene_bytes) return false;
    std::ifstream in(path, std::ios::binary);
    if (!in) return false;
    const std::string text{std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>()};
    const Json doc = Json::parse(text, nullptr, /*allow_exceptions=*/false);
    return !doc.is_discarded() && doc.is_object() && doc.contains("entities") &&
           doc["entities"].is_array();
}

RpcResult list_models(Context& ctx, const Json& params) {
    core::JsonFields f(params, "");
    if (auto ok = finish(f); !ok) return std::unexpected(ok.error());
    if (!ctx.services.project_root) return unavailable("project file access");
    const fs::path root = ctx.services.project_root();
    const fs::path assets = root / "assets";

    std::vector<std::string> models;
    std::error_code ec;
    bool truncated = false;
    fs::recursive_directory_iterator it(assets, fs::directory_options::skip_permission_denied, ec);
    for (const fs::recursive_directory_iterator end; !ec && it != end; it.increment(ec)) {
        std::error_code type_ec;
        if (!it->is_regular_file(type_ec) || !is_model(it->path())) continue;
        if (models.size() == k_max_models) {
            truncated = true;
            break;
        }
        models.push_back(it->path().lexically_relative(root).generic_string());
    }
    std::sort(models.begin(), models.end());
    Json out{{"models", models}};
    if (truncated) out["truncated"] = true;
    return out;
}

RpcResult spawn_model(Context& ctx, const Json& params) {
    scene::Scene& sc = *ctx.scene;
    entt::registry& r = sc.registry();
    core::JsonFields f(params, "");
    std::string relative;
    auto path = project_path(ctx, f, "path", relative);
    std::string name;
    const bool has_name = f.has("name");
    f.string("name", name, 256);
    const Json* parent_ref = f.raw("parent");
    const Json* components = f.raw("components");
    if (auto ok = finish(f); !ok) return std::unexpected(ok.error());
    if (!path) return std::unexpected(path.error());
    if (!is_model(*path)) return bad_params("path: expected a .gltf or .glb file");
    std::error_code ec;
    if (!fs::is_regular_file(*path, ec)) {
        return rpc_fail(rpc_code::not_found, "path: no such file '" + relative + "'");
    }
    entt::entity parent = entt::null;
    if (parent_ref != nullptr) {
        auto p = resolve_parent(r, *parent_ref, "parent");
        if (!p) return std::unexpected(p.error());
        parent = *p;
    }
    const bool has_components = components != nullptr && !components->is_null();
    if (has_components) { // validate before uploading anything
        entt::registry scratch;
        if (auto ok = apply_components_atomic(scratch, scratch.create(), *components); !ok) {
            return std::unexpected(ok.error());
        }
    }
    if (!ctx.services.spawn_model) return unavailable("glTF loading");

    auto roots = ctx.services.spawn_model(*path);
    if (!roots) return rpc_fail(rpc_code::failed, "spawn failed: " + roots.error().message);
    // Several roots get a common group entity, so name/parent/components
    // apply to the model as a whole.
    std::vector<entt::entity> top = *roots;
    if (top.size() > 1 && (has_name || parent != entt::null || has_components)) {
        const entt::entity group =
            sc.create_entity(has_name ? name : path->stem().string());
        for (const entt::entity e : top) (void)sc.set_parent(e, group);
        top = {group};
    }
    if (top.size() == 1) {
        const entt::entity e = top.front();
        if (has_name) r.emplace_or_replace<scene::Name>(e, name);
        if (has_components) {
            if (auto ok = apply_components_atomic(r, e, *components); !ok) {
                return std::unexpected(ok.error());
            }
        }
        if (parent != entt::null && !sc.set_parent(e, parent)) {
            return rpc_fail(rpc_code::failed, "parent: cannot parent the model there");
        }
    }
    Json ids = Json::array();
    for (const entt::entity e : top) {
        refresh_world(r, e);
        ids.push_back(entity_json(e));
    }
    return Json{{"roots", std::move(ids)}};
}

RpcResult set_playing(Context& ctx, const Json& params, bool play) {
    core::JsonFields f(params, "");
    if (auto ok = finish(f); !ok) return std::unexpected(ok.error());
    if (!ctx.services.set_playing) return unavailable("play mode");
    auto playing = ctx.services.set_playing(play);
    if (!playing) return rpc_fail(rpc_code::failed, playing.error().message);
    return Json{{"playing", *playing}};
}

RpcResult save_scene(Context& ctx, const Json& params) {
    core::JsonFields f(params, "");
    std::string relative;
    auto path = project_path(ctx, f, "path", relative);
    if (auto ok = finish(f); !ok) return std::unexpected(ok.error());
    if (!path) return std::unexpected(path.error());
    if (lower_extension(*path) != ".json") return bad_params("path: scene files end in .json");
    std::error_code ec;
    if (fs::exists(*path, ec) && !looks_like_scene_file(*path)) {
        return bad_params("path: '" + relative +
                          "' exists and is not a scene file; refusing to overwrite it");
    }
    if (!ctx.services.save_scene) return unavailable("scene saving");
    if (auto ok = ctx.services.save_scene(*path); !ok) {
        return rpc_fail(rpc_code::failed, "save failed: " + ok.error().message);
    }
    return Json{{"path", relative}};
}

RpcResult load_scene(Context& ctx, const Json& params) {
    core::JsonFields f(params, "");
    std::string relative;
    auto path = project_path(ctx, f, "path", relative);
    if (auto ok = finish(f); !ok) return std::unexpected(ok.error());
    if (!path) return std::unexpected(path.error());
    std::error_code ec;
    if (!fs::is_regular_file(*path, ec)) {
        return rpc_fail(rpc_code::not_found, "path: no such file '" + relative + "'");
    }
    if (!ctx.services.load_scene) return unavailable("scene loading");
    if (auto ok = ctx.services.load_scene(*path); !ok) {
        return rpc_fail(rpc_code::failed, "load failed: " + ok.error().message);
    }
    return Json{{"path", relative}};
}

RpcResult nav_bake(Context& ctx, const Json& params) {
    core::JsonFields f(params, "");
    if (auto ok = finish(f); !ok) return std::unexpected(ok.error());
    if (!ctx.services.nav_bake) return unavailable("navmesh baking");
    auto polygons = ctx.services.nav_bake();
    if (!polygons) return rpc_fail(rpc_code::failed, "bake failed: " + polygons.error().message);
    return Json{{"polygons", *polygons}};
}

} // namespace

void register_app_commands(RpcServer& server, const ContextPtr& ctx) {
    server.add_method("asset.list_models",
                      "glTF/GLB models under assets/: {models:[project-relative paths]}.",
                      [ctx](const Json& p) { return list_models(*ctx, p); });
    server.add_method("asset.spawn_model",
                      "Instantiate a model: {path, name?, parent?, components?} -> {roots:[ids]}. "
                      "A multi-root model is grouped under one entity when name/parent/"
                      "components are given.",
                      [ctx](const Json& p) { return spawn_model(*ctx, p); });
    server.add_method("sim.play", "Enter play mode (physics, AI): {playing}.",
                      [ctx](const Json& p) { return set_playing(*ctx, p, true); });
    server.add_method("sim.stop", "Leave play mode, restoring the edited scene: {playing}.",
                      [ctx](const Json& p) { return set_playing(*ctx, p, false); });
    server.add_method("scene.save",
                      "Save the scene: {path} (project-relative .json; only scene files are "
                      "overwritten) -> {path}.",
                      [ctx](const Json& p) { return save_scene(*ctx, p); });
    server.add_method("scene.load", "Replace the scene with a saved one: {path} -> {path}.",
                      [ctx](const Json& p) { return load_scene(*ctx, p); });
    server.add_method("nav.bake",
                      "Bake the navmesh from terrain + static colliders: {polygons}.",
                      [ctx](const Json& p) { return nav_bake(*ctx, p); });
}

} // namespace engine::remote::world
