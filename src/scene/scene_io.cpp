#include "engine/scene/scene_io.hpp"

#include <fstream>
#include <set>
#include <string>
#include <unordered_map>
#include <vector>

#include <entt/entity/registry.hpp>
#include <glm/glm.hpp>
#include <nlohmann/json.hpp>

#include "engine/asset/asset_manager.hpp"
#include "engine/scene/components.hpp"
#include "engine/scene/gltf_loader.hpp"
#include "engine/scene/scene.hpp"

namespace engine::scene {

namespace {

using nlohmann::json;

[[nodiscard]] std::unexpected<core::Error> fail(std::string what) {
    return std::unexpected(core::Error{std::move(what)});
}

[[nodiscard]] json mat4_to_json(const glm::mat4& m) {
    json out = json::array();
    for (int c = 0; c < 4; ++c) {
        for (int r = 0; r < 4; ++r) out.push_back(m[c][r]);
    }
    return out;
}

[[nodiscard]] glm::mat4 mat4_from_json(const json& a) {
    glm::mat4 m(1.0F);
    if (a.is_array() && a.size() == 16) {
        for (int c = 0; c < 4; ++c) {
            for (int r = 0; r < 4; ++r) m[c][r] = a[std::size_t(c * 4 + r)].get<float>();
        }
    }
    return m;
}

[[nodiscard]] json vec3_to_json(const glm::vec3& v) { return json::array({v.x, v.y, v.z}); }
[[nodiscard]] glm::vec3 vec3_from_json(const json& a, glm::vec3 fallback = glm::vec3(0.0F)) {
    if (a.is_array() && a.size() == 3) {
        return {a[0].get<float>(), a[1].get<float>(), a[2].get<float>()};
    }
    return fallback;
}

} // namespace

std::expected<void, core::Error> save_scene(const Scene& scene,
                                            const std::filesystem::path& path) {
    const entt::registry& r = scene.registry();

    // Stable index per live entity (file order). Parent links become indices.
    std::vector<entt::entity> order;
    std::unordered_map<entt::entity, int> index_of;
    for (const entt::entity e : r.view<entt::entity>()) {
        index_of.emplace(e, static_cast<int>(order.size()));
        order.push_back(e);
    }

    json entities = json::array();
    for (const entt::entity e : order) {
        json je;
        if (const auto* name = r.try_get<Name>(e)) je["name"] = name->value;
        if (const auto* t = r.try_get<Transform>(e)) je["transform"] = mat4_to_json(t->local);
        int parent = -1;
        if (const auto* p = r.try_get<Parent>(e);
            p != nullptr && p->entity != entt::null && index_of.contains(p->entity)) {
            parent = index_of.at(p->entity);
        }
        je["parent"] = parent;

        if (const auto* src = r.try_get<MeshSource>(e)) {
            je["mesh"] = {{"mesh_key", src->mesh_key},
                          {"material_key", src->material_key},
                          {"gltf", src->gltf_path}};
        }
        if (const auto* c = r.try_get<Collider>(e)) {
            je["collider"] = {{"shape", static_cast<int>(c->shape)},
                              {"half_extents", vec3_to_json(c->half_extents)},
                              {"offset", vec3_to_json(c->offset)},
                              {"sensor", c->is_sensor}};
        }
        if (const auto* b = r.try_get<RigidBody>(e)) {
            je["rigidbody"] = {{"motion", static_cast<int>(b->motion)}, {"mass", b->mass}};
        }
        if (const auto* l = r.try_get<DirectionalLight>(e)) {
            je["dir_light"] = {{"direction", vec3_to_json(l->direction)},
                               {"color", vec3_to_json(l->color)},
                               {"intensity", l->intensity}};
        }
        if (const auto* l = r.try_get<PointLight>(e)) {
            je["point_light"] = {{"color", vec3_to_json(l->color)},
                                 {"radius", l->radius},
                                 {"intensity", l->intensity}};
        }
        if (const auto* c = r.try_get<Camera>(e)) {
            je["camera"] = {{"fov_y", c->fov_y},
                            {"near", c->near_z},
                            {"far", c->far_z},
                            {"active", c->is_active}};
        }
        if (const auto* t = r.try_get<Terrain>(e)) {
            je["terrain"] = {{"seed", t->params.seed},
                             {"resolution", t->params.resolution},
                             {"tile_size", t->params.tile_size_m},
                             {"height", t->params.height_m},
                             {"octaves", t->params.octaves},
                             {"warp", t->params.warp_strength},
                             {"droplets", t->params.erosion_droplets},
                             {"snowline", t->snowline_m},
                             {"streaming", t->streaming},
                             {"stream_radius", t->stream_radius}};
        }
        entities.push_back(std::move(je));
    }

    json root;
    root["version"] = 1;
    root["entities"] = std::move(entities);

    std::error_code ec;
    if (path.has_parent_path()) std::filesystem::create_directories(path.parent_path(), ec);
    std::ofstream out(path);
    if (!out) return fail("save_scene: cannot open '" + path.string() + "' for writing");
    out << root.dump(2);
    if (!out.good()) return fail("save_scene: write failed for '" + path.string() + "'");
    return {};
}

std::expected<void, core::Error>
load_scene(Scene& scene, const std::filesystem::path& path, rhi::GpuAllocator& allocator,
           const rhi::TransferContext& transfer, asset::AssetManager& assets) {
    std::ifstream in(path);
    if (!in) return fail("load_scene: cannot open '" + path.string() + "'");
    json root = json::parse(in, nullptr, /*allow_exceptions=*/false);
    if (root.is_discarded() || !root.contains("entities") || !root["entities"].is_array()) {
        return fail("load_scene: '" + path.string() + "' is not a scene file");
    }

    // Pre-load every referenced glTF: instantiating into a scratch scene fills
    // the AssetManager cache with the exact keys the entities reference; the
    // scratch entities are discarded with the scratch scene.
    std::set<std::string> gltf_paths;
    for (const json& je : root["entities"]) {
        if (je.contains("mesh") && je["mesh"].contains("gltf")) {
            const std::string p = je["mesh"]["gltf"].get<std::string>();
            if (!p.empty()) gltf_paths.insert(p);
        }
    }
    // The scene file does not persist SkinnedMesh/Animator (asset handles are
    // not serializable); harvest them from the scratch instantiation instead,
    // keyed by primitive mesh key, and re-attach below. root_transform is
    // entity-relative, so the copied value is valid wherever the saved entity
    // ended up.
    std::unordered_map<std::string, SkinnedMesh> skin_by_mesh_key;
    std::unordered_map<std::string, Animator> animator_by_mesh_key;
    for (const std::string& p : gltf_paths) {
        Scene scratch;
        if (auto roots = GltfLoader::instantiate(p, allocator, transfer, assets, scratch);
            !roots) {
            return fail("load_scene: glTF '" + p + "' failed: " + roots.error().message);
        }
        for (auto [se, ssrc, sskin] :
             scratch.registry().view<MeshSource, SkinnedMesh>().each()) {
            SkinnedMesh copy = sskin;
            copy.joint_matrices.clear(); // rewritten by the animation system
            skin_by_mesh_key.emplace(ssrc.mesh_key, std::move(copy));
            if (const auto* sanim = scratch.registry().try_get<Animator>(se)) {
                animator_by_mesh_key.emplace(ssrc.mesh_key, *sanim);
            }
        }
    }
    // Cache-hit-only loaders: the asset must already exist under the key.
    const auto cached_mesh = [&](const std::string& key) {
        return assets.load<asset::MeshAsset>(
            key, [](const std::filesystem::path&) -> std::expected<asset::MeshAsset, core::Error> {
                return std::unexpected(core::Error{"asset key not in cache"});
            });
    };
    const auto cached_material = [&](const std::string& key) {
        return assets.load<asset::MaterialAsset>(
            key,
            [](const std::filesystem::path&) -> std::expected<asset::MaterialAsset, core::Error> {
                return std::unexpected(core::Error{"asset key not in cache"});
            });
    };

    entt::registry& r = scene.registry();
    r.clear();

    const json& jents = root["entities"];
    std::vector<entt::entity> created;
    created.reserve(jents.size());
    for (const json& je : jents) {
        const entt::entity e =
            scene.create_entity(je.contains("name") ? je["name"].get<std::string>() : "");
        created.push_back(e);
        if (je.contains("transform")) {
            r.get<Transform>(e).local = mat4_from_json(je["transform"]);
        }
        if (je.contains("mesh")) {
            const json& jm = je["mesh"];
            MeshSource src;
            src.mesh_key = jm.value("mesh_key", "");
            src.material_key = jm.value("material_key", "");
            src.gltf_path = jm.value("gltf", "");
            MeshRenderer mr;
            if (!src.mesh_key.empty()) mr.mesh = cached_mesh(src.mesh_key);
            if (!src.material_key.empty()) mr.material = cached_material(src.material_key);
            if (!mr.mesh.is_loaded()) {
                std::fprintf(stderr, "[scene] mesh key '%s' not resolvable; entity is mesh-less\n",
                             src.mesh_key.c_str());
            }
            r.emplace<MeshRenderer>(e, std::move(mr));
            // Re-adopt the skin/animator this primitive carries in its source
            // glTF (see the scratch harvest above).
            if (auto it = skin_by_mesh_key.find(src.mesh_key); it != skin_by_mesh_key.end()) {
                r.emplace<SkinnedMesh>(e, it->second);
                if (auto ai = animator_by_mesh_key.find(src.mesh_key);
                    ai != animator_by_mesh_key.end()) {
                    r.emplace<Animator>(e, ai->second);
                }
            }
            r.emplace<MeshSource>(e, std::move(src));
        }
        if (je.contains("collider")) {
            const json& jc = je["collider"];
            Collider c;
            c.shape = static_cast<ColliderShape>(jc.value("shape", 0));
            c.half_extents = vec3_from_json(jc["half_extents"], glm::vec3(0.5F));
            c.offset = vec3_from_json(jc["offset"]);
            c.is_sensor = jc.value("sensor", false);
            r.emplace<Collider>(e, c);
        }
        if (je.contains("rigidbody")) {
            const json& jb = je["rigidbody"];
            r.emplace<RigidBody>(e, static_cast<BodyMotion>(jb.value("motion", 2)),
                                 jb.value("mass", 1.0F));
        }
        if (je.contains("dir_light")) {
            const json& jl = je["dir_light"];
            r.emplace<DirectionalLight>(e, vec3_from_json(jl["direction"], {0, -1, 0}),
                                        vec3_from_json(jl["color"], glm::vec3(1.0F)),
                                        jl.value("intensity", 1.0F));
        }
        if (je.contains("point_light")) {
            const json& jl = je["point_light"];
            r.emplace<PointLight>(e, vec3_from_json(jl["color"], glm::vec3(1.0F)),
                                  jl.value("radius", 10.0F), jl.value("intensity", 1.0F));
        }
        if (je.contains("camera")) {
            const json& jc = je["camera"];
            Camera c;
            c.fov_y = jc.value("fov_y", 1.0F);
            c.near_z = jc.value("near", 0.1F);
            c.far_z = jc.value("far", 1000.0F);
            c.is_active = jc.value("active", true);
            r.emplace<Camera>(e, c);
        }
        if (je.contains("terrain")) {
            const json& jt = je["terrain"];
            Terrain t;
            t.params.seed = jt.value("seed", 1337U);
            t.params.resolution = jt.value("resolution", 1025U);
            t.params.tile_size_m = jt.value("tile_size", 2000.0F);
            t.params.height_m = jt.value("height", 180.0F);
            t.params.octaves = jt.value("octaves", 7);
            t.params.warp_strength = jt.value("warp", 0.30F);
            t.params.erosion_droplets = jt.value("droplets", 120'000U);
            t.snowline_m = jt.value("snowline", 110.0F);
            t.streaming = jt.value("streaming", false);
            t.stream_radius = jt.value("stream_radius", 1);
            t.regenerate = true; // rebuild on load
            r.emplace<Terrain>(e, t);
        }
    }

    // Hierarchy second pass (set_parent maintains the Children lists).
    for (std::size_t i = 0; i < created.size(); ++i) {
        const int parent = jents[i].value("parent", -1);
        if (parent >= 0 && static_cast<std::size_t>(parent) < created.size()) {
            scene.set_parent(created[i], created[static_cast<std::size_t>(parent)]);
        }
    }

    scene.tick(0.0F); // world transforms valid on return
    return {};
}

std::expected<void, core::Error>
run_scene_io_self_test(rhi::GpuAllocator& allocator, const rhi::TransferContext& transfer) {
    asset::AssetManager assets;

    // A triangle mesh under a code-created cache key (the showcase pattern).
    auto mesh = assets.load<asset::MeshAsset>(
        "scene_io_test/tri", [&](const std::filesystem::path&) {
            MeshData d;
            d.vertices.resize(3);
            d.vertices[0].position = {0.0F, 0.0F, 0.0F};
            d.vertices[1].position = {1.0F, 0.0F, 0.0F};
            d.vertices[2].position = {0.0F, 1.0F, 0.0F};
            d.indices = {0, 1, 2};
            d.submeshes = {asset::SubMesh{0, 3, 0}};
            return upload_mesh(d, allocator, transfer);
        });
    if (!mesh.is_loaded()) return fail("scene io self-test: test mesh upload failed");

    Scene scene;
    entt::registry& r = scene.registry();
    const entt::entity parent = scene.create_entity("parent");
    r.get<Transform>(parent).local = glm::mat4(2.0F);
    r.emplace<MeshRenderer>(parent, mesh, asset::AssetHandle<asset::MaterialAsset>{});
    r.emplace<MeshSource>(parent, "scene_io_test/tri", "", "");
    r.emplace<Collider>(parent, ColliderShape::capsule, glm::vec3(0.4F, 0.9F, 0.4F),
                        glm::vec3(0.0F, 0.1F, 0.0F), true);
    r.emplace<RigidBody>(parent, BodyMotion::kinematic, 4.0F);
    const entt::entity child = scene.create_entity("child");
    scene.set_parent(child, parent);
    r.emplace<PointLight>(child, glm::vec3(1.0F, 0.2F, 0.1F), 6.0F, 2.0F);

    const std::filesystem::path file =
        std::filesystem::temp_directory_path() / "quat3d_scene_io_test.json";
    if (auto s = save_scene(scene, file); !s) return std::unexpected(s.error());

    if (auto l = load_scene(scene, file, allocator, transfer, assets); !l) {
        return std::unexpected(l.error());
    }
    std::error_code ec;
    std::filesystem::remove(file, ec);

    if (r.view<Transform>().size() != 2) {
        return fail("scene io self-test: entity count after load");
    }
    entt::entity loaded_parent = entt::null;
    entt::entity loaded_child = entt::null;
    for (const auto [e, name] : r.view<Name>().each()) {
        if (name.value == "parent") loaded_parent = e;
        if (name.value == "child") loaded_child = e;
    }
    if (loaded_parent == entt::null || loaded_child == entt::null) {
        return fail("scene io self-test: names lost");
    }
    if (const auto* p = r.try_get<Parent>(loaded_child);
        p == nullptr || p->entity != loaded_parent) {
        return fail("scene io self-test: hierarchy lost");
    }
    if (r.get<Transform>(loaded_parent).local[0][0] != 2.0F) {
        return fail("scene io self-test: transform lost");
    }
    const auto* col = r.try_get<Collider>(loaded_parent);
    if (col == nullptr || col->shape != ColliderShape::capsule || !col->is_sensor ||
        col->offset.y != 0.1F) {
        return fail("scene io self-test: collider lost");
    }
    const auto* rb = r.try_get<RigidBody>(loaded_parent);
    if (rb == nullptr || rb->motion != BodyMotion::kinematic || rb->mass != 4.0F) {
        return fail("scene io self-test: rigid body lost");
    }
    const auto* mr = r.try_get<MeshRenderer>(loaded_parent);
    if (mr == nullptr || !mr->mesh.is_loaded() || mr->mesh->vertex_count != 3) {
        return fail("scene io self-test: mesh reference not re-resolved");
    }
    const auto* pl = r.try_get<PointLight>(loaded_child);
    if (pl == nullptr || pl->radius != 6.0F) {
        return fail("scene io self-test: point light lost");
    }
    return {};
}

} // namespace engine::scene
