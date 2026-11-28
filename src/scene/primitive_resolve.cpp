// Primitive -> MeshRenderer resolution — see include/engine/scene/primitives.hpp.

#include <cstdio>
#include <filesystem>
#include <string>
#include <string_view>
#include <vector>

#include <entt/entity/registry.hpp>

#include "engine/asset/asset_manager.hpp"
#include "engine/asset/material_asset.hpp"
#include "engine/rhi/gpu_allocator.hpp"
#include "engine/scene/primitives.hpp"
#include "fnv_hash.hpp"

namespace engine::scene {

namespace {

[[nodiscard]] std::uint64_t material_hash(const asset::PbrMaterialParams& p,
                                          std::string_view shader) {
    Fnv64 h;
    for (int i = 0; i < 4; ++i) h.f32(p.base_color_factor[i]);
    for (int i = 0; i < 3; ++i) h.f32(p.emissive_factor[i]);
    h.f32(p.metallic_factor);
    h.f32(p.roughness_factor);
    h.u32(p.flags);
    if (!shader.empty()) { // keeps the keys of shader-less materials unchanged
        h.str(shader);
        for (const glm::vec4& v : p.shader_params) {
            for (int i = 0; i < 4; ++i) h.f32(v[i]);
        }
    }
    return h.value();
}

[[nodiscard]] const char* shape_name(PrimitiveShape shape) {
    switch (shape) {
    case PrimitiveShape::cube: return "cube";
    case PrimitiveShape::sphere: return "sphere";
    case PrimitiveShape::plane: return "plane";
    case PrimitiveShape::cylinder: return "cylinder";
    case PrimitiveShape::cone: return "cone";
    case PrimitiveShape::capsule: return "capsule";
    }
    return "cube";
}

[[nodiscard]] bool needs_resolve(const entt::registry& registry, entt::entity e,
                                 const Primitive& p) {
    return p.resolved != primitive_hash(p) || !registry.all_of<MeshRenderer>(e);
}

} // namespace

asset::PbrMaterialParams primitive_material_params(const Primitive& primitive) {
    asset::PbrMaterialParams p;
    p.base_color_factor = primitive.color;
    p.emissive_factor = glm::vec4(primitive.emissive, 0.0F);
    p.metallic_factor = primitive.metallic;
    p.roughness_factor = primitive.roughness;
    p.flags = 0;
    // Authored material shaders run in the deferred pass only: their alpha is
    // a cutout there, so the material never routes to the blended pass.
    if (primitive.color.a < 1.0F && primitive.shader.empty()) p.flags |= asset::material_blend;
    p.shader_params = primitive.shader_params;
    // A plane has one face: render its back too, or it vanishes when the
    // camera dips below it (and casts no shadow from below).
    if (primitive.shape == PrimitiveShape::plane) p.flags |= asset::material_double_sided;
    return p;
}

std::uint64_t primitive_hash(const Primitive& primitive) {
    Fnv64 h;
    h.u32(static_cast<std::uint32_t>(primitive.shape));
    h.u64(material_hash(primitive_material_params(primitive), primitive.shader));
    return h.nonzero();
}

bool primitives_need_resolve(const entt::registry& registry) {
    for (auto [e, p] : registry.view<const Primitive>().each()) {
        if (needs_resolve(registry, e, p)) return true;
    }
    return false;
}

PrimitiveAssets load_primitive_assets(const Primitive& primitive, rhi::GpuAllocator& allocator,
                                      const rhi::TransferContext& transfer,
                                      asset::AssetManager& assets) {
    PrimitiveAssets out;
    out.mesh_key = std::string("primitive/mesh/") + shape_name(primitive.shape);
    out.mesh = assets.load<asset::MeshAsset>(out.mesh_key, [&](const std::filesystem::path&) {
        return upload_mesh(make_primitive_data(primitive.shape), allocator, transfer);
    });

    out.material = load_primitive_material(primitive, allocator, transfer, assets, &out.material_key);
    return out;
}

asset::AssetHandle<asset::MaterialAsset> load_primitive_material(const Primitive& primitive,
                                                                 rhi::GpuAllocator& allocator,
                                                                 const rhi::TransferContext& transfer,
                                                                 asset::AssetManager& assets,
                                                                 std::string* key_out) {
    const asset::PbrMaterialParams params = primitive_material_params(primitive);
    char hex[17];
    std::snprintf(hex, sizeof hex, "%016llx",
                  static_cast<unsigned long long>(material_hash(params, primitive.shader)));
    const std::string key = std::string("primitive/material/") + hex;
    if (key_out != nullptr) *key_out = key;
    return assets.load<asset::MaterialAsset>(key, [&](const std::filesystem::path&) {
        auto material = asset::upload_material(params, asset::MaterialTextures{}, allocator, transfer);
        if (material) material->shader = primitive.shader;
        return material;
    });
}

std::expected<std::size_t, core::Error>
resolve_primitives(entt::registry& registry, rhi::GpuAllocator& allocator,
                   const rhi::TransferContext& transfer, asset::AssetManager& assets) {
    std::vector<entt::entity> stale;
    for (auto [e, p] : registry.view<const Primitive>().each()) {
        if (needs_resolve(registry, e, p)) stale.push_back(e);
    }

    std::string failures;
    for (const entt::entity e : stale) {
        Primitive& p = registry.get<Primitive>(e);
        PrimitiveAssets a = load_primitive_assets(p, allocator, transfer, assets);
        if (!a.mesh.is_loaded() && failures.find(a.mesh_key) == std::string::npos) {
            failures += (failures.empty() ? "" : ", ") + a.mesh_key;
        }
        if (!a.material.is_loaded() && failures.find(a.material_key) == std::string::npos) {
            failures += (failures.empty() ? "" : ", ") + a.material_key;
        }
        registry.emplace_or_replace<MeshRenderer>(e, std::move(a.mesh), std::move(a.material));
        // Recorded even on failure, so a failing upload (out of device
        // memory) is not retried every frame; the next edit of the primitive
        // resolves it again, and the AssetManager retries the failed slot.
        p.resolved = primitive_hash(p);
    }
    if (!failures.empty()) {
        return std::unexpected(core::Error{"primitive asset upload failed: " + failures});
    }
    return stale.size();
}

} // namespace engine::scene
