#include "engine/scene/scene_io.hpp"

#include <cstdio>
#include <fstream>
#include <optional>
#include <set>
#include <string>
#include <unordered_map>
#include <vector>

#include <entt/entity/registry.hpp>
#include <glm/glm.hpp>
#include <nlohmann/json.hpp>

#include "engine/asset/asset_manager.hpp"
#include "engine/core/json_fields.hpp"
#include "engine/scene/component_json.hpp"
#include "engine/scene/components.hpp"
#include "engine/scene/gltf_loader.hpp"
#include "engine/scene/scene.hpp"

namespace engine::scene {

namespace {

using nlohmann::json;

constexpr int k_scene_version = 2;

[[nodiscard]] std::unexpected<core::Error> fail(std::string what) {
    return std::unexpected(core::Error{std::move(what)});
}

// Entity-level keys handled by scene_io itself; every other key is a
// component decoded by the shared codec.
[[nodiscard]] bool is_entity_key(std::string_view key) {
    return key == "name" || key == "parent" || key == "mesh";
}

struct MeshRef {
    std::string mesh_key;
    std::string material_key;
    std::string gltf;
};

// Validated view of one file entity (before anything touches a scene).
struct FileEntity {
    std::string            name;
    int                    parent = -1;
    std::optional<MeshRef> mesh;
    const json*            object = nullptr;
};

[[nodiscard]] std::expected<std::vector<FileEntity>, core::Error> read_entities(const json& root) {
    core::JsonFields top(root, "scene", /*strict=*/false);
    int version = 1;
    top.integer("version", version, 1, 1000);
    const json* list = top.raw("entities");
    if (auto ok = top.finish(); !ok) return std::unexpected(ok.error());
    if (version > k_scene_version) {
        std::fprintf(stderr, "[scene] file version %d is newer than %d; loading what is known\n",
                     version, k_scene_version);
    }
    if (list == nullptr || !list->is_array()) return fail("scene: missing \"entities\" array");

    std::vector<FileEntity> out;
    out.reserve(list->size());
    for (std::size_t i = 0; i < list->size(); ++i) {
        const json& je = (*list)[i];
        const std::string where = "entities[" + std::to_string(i) + "]";
        core::JsonFields f(je, where, /*strict=*/false);
        FileEntity fe;
        fe.object = &je;
        f.string("name", fe.name, 1024);
        f.integer("parent", fe.parent, -1, static_cast<std::int64_t>(list->size()) - 1);
        if (const json* m = f.raw("mesh")) {
            core::JsonFields mf(*m, where + ".mesh", /*strict=*/false);
            MeshRef ref;
            mf.string("mesh_key", ref.mesh_key, 4096).string("material_key", ref.material_key, 4096);
            mf.string("gltf", ref.gltf, 4096);
            if (auto ok = mf.finish(); !ok) return std::unexpected(ok.error());
            fe.mesh = std::move(ref);
        }
        if (auto ok = f.finish(); !ok) return std::unexpected(ok.error());
        if (fe.parent == static_cast<int>(i)) return fail(where + ".parent: an entity cannot parent itself");
        out.push_back(std::move(fe));
    }
    return out;
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
        // Components via the shared codec (exact matrix for the transform),
        // plus the entity-level name / parent / mesh provenance.
        json je = encode_components(r, e, {.transform_as_matrix = true, .include_read_only = false});
        if (const auto* name = r.try_get<Name>(e)) je["name"] = name->value;
        int parent = -1;
        if (const auto* p = r.try_get<Parent>(e);
            p != nullptr && p->entity != entt::null && index_of.contains(p->entity)) {
            parent = index_of.at(p->entity);
        }
        je["parent"] = parent;
        // Primitives rebuild their own mesh; only glTF/code meshes need keys.
        if (const auto* src = r.try_get<MeshSource>(e); src != nullptr && !r.all_of<Primitive>(e)) {
            je["mesh"] = {{"mesh_key", src->mesh_key},
                          {"material_key", src->material_key},
                          {"gltf", src->gltf_path}};
        }
        entities.push_back(std::move(je));
    }

    json root;
    root["version"] = k_scene_version;
    root["entities"] = std::move(entities);

    std::error_code ec;
    if (path.has_parent_path()) std::filesystem::create_directories(path.parent_path(), ec);
    std::ofstream out(path);
    if (!out) return fail("save_scene: cannot open '" + path.string() + "' for writing");
    // Replace (not throw on) invalid UTF-8: names from glTF files are untrusted.
    out << root.dump(2, ' ', false, json::error_handler_t::replace);
    if (!out.good()) return fail("save_scene: write failed for '" + path.string() + "'");
    return {};
}

std::expected<void, core::Error>
load_scene(Scene& scene, const std::filesystem::path& path, rhi::GpuAllocator& allocator,
           const rhi::TransferContext& transfer, asset::AssetManager& assets) {
    std::ifstream in(path);
    if (!in) return fail("load_scene: cannot open '" + path.string() + "'");
    const json root = json::parse(in, nullptr, /*allow_exceptions=*/false);
    if (root.is_discarded()) return fail("load_scene: '" + path.string() + "' is not valid JSON");
    auto file_entities = read_entities(root);
    if (!file_entities) {
        return fail("load_scene: '" + path.string() + "': " + file_entities.error().message);
    }

    // Pre-load every referenced glTF: instantiating into a scratch scene fills
    // the AssetManager cache with the exact keys the entities reference; the
    // scratch entities are discarded with the scratch scene.
    std::set<std::string> gltf_paths;
    for (const FileEntity& fe : *file_entities) {
        if (fe.mesh && !fe.mesh->gltf.empty()) gltf_paths.insert(fe.mesh->gltf);
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

    // Build into a staging scene; the live scene is replaced only once the
    // whole file decoded, so a bad file leaves the current scene intact.
    Scene staging;
    entt::registry& r = staging.registry();
    std::vector<entt::entity> created;
    created.reserve(file_entities->size());
    for (std::size_t i = 0; i < file_entities->size(); ++i) {
        const FileEntity& fe = (*file_entities)[i];
        const entt::entity e = staging.create_entity(fe.name);
        created.push_back(e);
        for (const auto& [key, value] : fe.object->items()) {
            // Animator settings apply after the glTF re-attaches the Animator.
            if (is_entity_key(key) || key == "animator") continue;
            if (find_component_info(key) == nullptr) {
                std::fprintf(stderr, "[scene] entities[%zu]: ignoring unknown key '%s'\n", i,
                             key.c_str());
                continue;
            }
            if (auto ok = apply_component(r, e, key, value, /*strict=*/false); !ok) {
                return fail("load_scene: '" + path.string() + "': entities[" + std::to_string(i) +
                            "]: " + ok.error().message);
            }
        }
        if (auto* t = r.try_get<Terrain>(e)) t->regenerate = true; // rebuild on load
        if (fe.mesh) {
            MeshSource src{fe.mesh->mesh_key, fe.mesh->material_key, fe.mesh->gltf};
            // Cache-only lookups: get() never creates a slot, so a missing key
            // cannot leave a permanently failed entry behind (audit C16).
            MeshRenderer mr;
            if (!src.mesh_key.empty()) {
                mr.mesh = assets.get<asset::MeshAsset>(asset::asset_id_from_path(src.mesh_key));
            }
            if (!src.material_key.empty()) {
                mr.material =
                    assets.get<asset::MaterialAsset>(asset::asset_id_from_path(src.material_key));
            }
            if (!mr.mesh.valid() || !mr.mesh.is_loaded()) {
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
        if (const auto it = fe.object->find("animator");
            it != fe.object->end() && r.all_of<Animator>(e)) {
            if (auto ok = apply_component(r, e, "animator", *it, /*strict=*/false); !ok) {
                return fail("load_scene: '" + path.string() + "': entities[" +
                            std::to_string(i) + "]: " + ok.error().message);
            }
        }
    }

    // Hierarchy second pass (set_parent maintains the Children lists and
    // refuses cycles, which an edited file can contain).
    for (std::size_t i = 0; i < created.size(); ++i) {
        const int parent = (*file_entities)[i].parent;
        if (parent >= 0 &&
            !staging.set_parent(created[i], created[static_cast<std::size_t>(parent)])) {
            return fail("load_scene: '" + path.string() + "': entities[" + std::to_string(i) +
                        "].parent creates a cycle");
        }
    }

    staging.tick(0.0F); // world transforms valid on return
    scene = std::move(staging);
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
