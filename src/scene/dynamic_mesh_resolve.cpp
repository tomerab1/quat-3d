#include <format>
#include <string>

#include "engine/scene/dynamic_mesh_gpu.hpp"
#include "engine/scene/geometry_spec.hpp"
#include "engine/scene/primitives.hpp"
#include "scene/dynamic_mesh_store_impl.hpp"

namespace engine::scene {

namespace {

// Never 0, so a fresh component (generated_hash == 0) always runs.
std::uint64_t generator_hash(const DynamicMesh& d) {
    if (d.generator.is_null()) {
        return 0;
    }
    const std::string text = d.mesh + '\n' + d.generator.dump(-1, ' ', false, nlohmann::json::error_handler_t::replace);
    std::uint64_t h = 14695981039346656037ULL;
    for (const char c : text) {
        h = (h ^ static_cast<unsigned char>(c)) * 1099511628211ULL;
    }
    return h == 0 ? 1 : h;
}

std::uint64_t material_key(const DynamicMesh& d) {
    Primitive p = d.material;
    p.shape = PrimitiveShape::cube; // the shape plays no part in the material
    return primitive_hash(p);
}

bool generator_pending(const DynamicMesh& d, const DynamicMeshStore& store) {
    const std::uint64_t h = generator_hash(d);
    return h != 0 && (h != d.generated_hash || !store.contains(d.mesh));
}

bool renderer_stale(const entt::registry& r, entt::entity e, const DynamicMesh& d, const DynamicMeshStore& store) {
    std::uint64_t version = 0;
    const bool resident = dynamic_mesh_resident(store, d.mesh, &version).valid();
    if (!store.contains(d.mesh)) {
        return r.all_of<MeshRenderer>(e); // the mesh is gone: stop drawing it
    }
    if (!resident) {
        // Keep drawing the previous mesh until this one is resident; a
        // resident version without buffers is an empty mesh: stop drawing.
        return version != 0 && version != d.resolved_version && r.all_of<MeshRenderer>(e);
    }
    return version != d.resolved_version || material_key(d) != d.resolved_material || !r.all_of<MeshRenderer>(e);
}

} // namespace

bool dynamic_meshes_need_resolve(const entt::registry& registry, const DynamicMeshStore& store) {
    for (auto [e, d] : registry.view<const DynamicMesh>().each()) {
        if (generator_pending(d, store) || renderer_stale(registry, e, d, store)) {
            return true;
        }
    }
    return false;
}

std::expected<std::size_t, core::Error> resolve_dynamic_meshes(entt::registry& registry, DynamicMeshStore& store,
                                                               rhi::GpuAllocator& allocator,
                                                               const rhi::TransferContext& transfer,
                                                               asset::AssetManager& assets) {
    std::string failures;
    std::size_t updated = 0;
    for (auto [e, d] : registry.view<DynamicMesh>().each()) {
        if (generator_pending(d, store)) {
            // Recorded before running, so a failing spec is not retried every
            // frame; editing it (or the id) runs it again.
            d.generated_hash = generator_hash(d);
            auto mesh = generate_from_spec(d.generator, std::format("dynamic_mesh \"{}\".generator", d.mesh));
            std::expected<std::uint64_t, core::Error> submitted =
                mesh ? store.submit(d.mesh, std::move(*mesh)) : std::unexpected(mesh.error());
            if (!submitted) {
                failures += (failures.empty() ? "" : "; ") + submitted.error().message;
            }
        }
        if (!renderer_stale(registry, e, d, store)) {
            continue;
        }
        std::uint64_t version = 0;
        asset::AssetHandle<asset::MeshAsset> mesh = dynamic_mesh_resident(store, d.mesh, &version);
        if (!mesh.valid()) {
            registry.remove<MeshRenderer>(e);
            d.resolved_version = version;
            ++updated;
            continue;
        }
        auto material = load_primitive_material(d.material, allocator, transfer, assets);
        registry.emplace_or_replace<MeshRenderer>(e, std::move(mesh), std::move(material));
        d.resolved_version = version;
        d.resolved_material = material_key(d);
        ++updated;
    }
    if (!failures.empty()) {
        return std::unexpected(core::make_error(failures));
    }
    return updated;
}

} // namespace engine::scene
