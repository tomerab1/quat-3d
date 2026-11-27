// InstancedMesh -> InstancedMeshParts resolution — see
// include/engine/scene/instanced_mesh.hpp.

#include <map>
#include <string>
#include <utility>
#include <vector>

#include <entt/entity/registry.hpp>

#include "engine/asset/asset_manager.hpp"
#include "engine/rhi/gpu_allocator.hpp"
#include "engine/scene/gltf_loader.hpp"
#include "engine/scene/instanced_mesh.hpp"
#include "engine/scene/primitives.hpp"
#include "engine/scene/scene.hpp"

namespace engine::scene {

namespace {

[[nodiscard]] std::unexpected<core::Error> fail(std::string what) {
    return std::unexpected(core::Error{std::move(what)});
}

// glTF files instantiated during one resolve pass (several entities often
// instance the same model); asset uploads are cached by the AssetManager
// across passes anyway.
using ScratchScenes = std::map<std::string, std::expected<Scene, core::Error>>;

[[nodiscard]] const std::expected<Scene, core::Error>&
scratch_for(ScratchScenes& cache, const std::filesystem::path& path, rhi::GpuAllocator& allocator,
            const rhi::TransferContext& transfer, asset::AssetManager& assets) {
    const std::string key = path.string();
    if (auto it = cache.find(key); it != cache.end()) return it->second;
    Scene scratch;
    auto roots = GltfLoader::instantiate(path, allocator, transfer, assets, scratch);
    if (!roots) return cache.emplace(key, std::unexpected(roots.error())).first->second;
    transform_system(scratch.registry());
    return cache.emplace(key, std::move(scratch)).first->second;
}

// The static meshes of `scene` (or of the subtree rooted at the node named
// `node`), relative to that node.
[[nodiscard]] std::expected<std::vector<InstancedMeshPart>, core::Error>
gltf_parts(const Scene& scene, const std::string& node) {
    const entt::registry& r = scene.registry();
    glm::mat4 frame(1.0F);
    std::vector<entt::entity> subset;
    if (node.empty()) {
        for (const entt::entity e : r.view<const MeshRenderer>()) subset.push_back(e);
    } else {
        entt::entity root = entt::null;
        for (auto [e, name] : r.view<const Name>().each()) {
            if (name.value == node) {
                root = e;
                break;
            }
        }
        if (root == entt::null) return fail("no node named '" + node + "'");
        if (const auto* t = r.try_get<Transform>(root)) frame = glm::inverse(t->world);
        // Iterative walk; the loader builds an acyclic hierarchy, the bound
        // guards against a corrupt one regardless.
        std::vector<entt::entity> stack{root};
        constexpr std::size_t k_max_nodes = 1'000'000;
        while (!stack.empty() && subset.size() < k_max_nodes) {
            const entt::entity e = stack.back();
            stack.pop_back();
            subset.push_back(e);
            if (const auto* kids = r.try_get<Children>(e)) {
                for (const entt::entity c : kids->entities) {
                    if (r.valid(c)) stack.push_back(c);
                }
            }
        }
    }

    std::vector<InstancedMeshPart> parts;
    for (const entt::entity e : subset) {
        const auto* mr = r.try_get<MeshRenderer>(e);
        const auto* t = r.try_get<Transform>(e);
        if (mr == nullptr || t == nullptr || !mr->mesh.valid()) continue;
        if (r.all_of<SkinnedMesh>(e) || mr->mesh->skinned()) continue; // instances do not animate
        parts.push_back(InstancedMeshPart{mr->mesh, mr->material, frame * t->world});
    }
    if (parts.empty()) {
        return fail(node.empty() ? std::string("no static meshes")
                                 : "no static meshes under node '" + node + "'");
    }
    return parts;
}

} // namespace

std::expected<std::size_t, core::Error>
resolve_instanced_meshes(entt::registry& registry, rhi::GpuAllocator& allocator,
                         const rhi::TransferContext& transfer, asset::AssetManager& assets,
                         const std::filesystem::path& root) {
    std::vector<entt::entity> stale;
    for (auto [e, mesh] : registry.view<const InstancedMesh>().each()) {
        const auto* parts = registry.try_get<InstancedMeshParts>(e);
        if (parts == nullptr || parts->resolved != instanced_mesh_source_hash(mesh)) {
            stale.push_back(e);
        }
    }

    ScratchScenes scratch;
    std::string failures;
    const auto report = [&](const std::string& what) {
        if (failures.find(what) == std::string::npos) {
            failures += (failures.empty() ? "" : "; ") + what;
        }
    };
    for (const entt::entity e : stale) {
        const InstancedMesh& mesh = registry.get<InstancedMesh>(e);
        InstancedMeshParts parts;
        // Recorded even on failure: retrying an unchanged source every frame
        // could not succeed (the AssetManager caches failed slots too).
        parts.resolved = instanced_mesh_source_hash(mesh);
        if (mesh.gltf_path.empty()) {
            PrimitiveAssets a = load_primitive_assets(mesh.primitive, allocator, transfer, assets);
            if (!a.mesh.is_loaded()) report(a.mesh_key + " upload failed");
            if (!a.material.is_loaded()) report(a.material_key + " upload failed");
            parts.parts.push_back(
                InstancedMeshPart{std::move(a.mesh), std::move(a.material), glm::mat4(1.0F)});
        } else {
            std::filesystem::path path(mesh.gltf_path);
            if (path.is_relative() && !root.empty()) path = root / path;
            const auto& scene = scratch_for(scratch, path, allocator, transfer, assets);
            auto built = scene ? gltf_parts(*scene, mesh.gltf_node)
                               : std::expected<std::vector<InstancedMeshPart>, core::Error>(
                                     std::unexpected(scene.error()));
            if (built) {
                parts.parts = std::move(*built);
            } else {
                report("'" + mesh.gltf_path + "': " + built.error().message);
            }
        }
        parts.bounds = instanced_parts_bounds(parts.parts);
        registry.emplace_or_replace<InstancedMeshParts>(e, std::move(parts));
    }
    if (!failures.empty()) {
        return std::unexpected(core::Error{"instanced mesh resolve: " + failures});
    }
    return stale.size();
}

} // namespace engine::scene
