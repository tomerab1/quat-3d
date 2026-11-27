// Primitive -> MeshRenderer resolution — see include/engine/scene/primitives.hpp.

#include <cstdio>
#include <cstring>
#include <filesystem>
#include <string>
#include <vector>

#include <entt/entity/registry.hpp>

#include "engine/asset/asset_manager.hpp"
#include "engine/asset/material_asset.hpp"
#include "engine/rhi/gpu_allocator.hpp"
#include "engine/scene/primitives.hpp"

namespace engine::scene {

namespace {

// FNV-1a, 64-bit. Floats are hashed by value with -0 folded into +0 so equal
// colours always share a material.
class Fnv64 {
public:
    void bytes(const void* data, std::size_t size) {
        const auto* p = static_cast<const unsigned char*>(data);
        for (std::size_t i = 0; i < size; ++i) h_ = (h_ ^ p[i]) * 1099511628211ULL;
    }
    void f32(float f) {
        f += 0.0F; // -0 -> +0
        std::uint32_t bits = 0;
        std::memcpy(&bits, &f, sizeof bits);
        bytes(&bits, sizeof bits);
    }
    void u32(std::uint32_t v) { bytes(&v, sizeof v); }
    [[nodiscard]] std::uint64_t value() const { return h_; }

private:
    std::uint64_t h_ = 14695981039346656037ULL;
};

[[nodiscard]] std::uint64_t material_hash(const asset::PbrMaterialParams& p) {
    Fnv64 h;
    for (int i = 0; i < 4; ++i) h.f32(p.base_color_factor[i]);
    for (int i = 0; i < 3; ++i) h.f32(p.emissive_factor[i]);
    h.f32(p.metallic_factor);
    h.f32(p.roughness_factor);
    h.u32(p.flags);
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
    if (primitive.color.a < 1.0F) p.flags |= asset::material_blend;
    // A plane has one face: render its back too, or it vanishes when the
    // camera dips below it (and casts no shadow from below).
    if (primitive.shape == PrimitiveShape::plane) p.flags |= asset::material_double_sided;
    return p;
}

std::uint64_t primitive_hash(const Primitive& primitive) {
    Fnv64 h;
    h.u32(static_cast<std::uint32_t>(primitive.shape));
    const std::uint64_t m = material_hash(primitive_material_params(primitive));
    h.bytes(&m, sizeof m);
    return h.value() == 0 ? 1 : h.value();
}

bool primitives_need_resolve(const entt::registry& registry) {
    for (auto [e, p] : registry.view<const Primitive>().each()) {
        if (needs_resolve(registry, e, p)) return true;
    }
    return false;
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
        const std::string mesh_key = std::string("primitive/mesh/") + shape_name(p.shape);
        auto mesh = assets.load<asset::MeshAsset>(mesh_key, [&](const std::filesystem::path&) {
            return upload_mesh(make_primitive_data(p.shape), allocator, transfer);
        });

        const asset::PbrMaterialParams params = primitive_material_params(p);
        char hex[17];
        std::snprintf(hex, sizeof hex, "%016llx",
                      static_cast<unsigned long long>(material_hash(params)));
        const std::string material_key = std::string("primitive/material/") + hex;
        auto material =
            assets.load<asset::MaterialAsset>(material_key, [&](const std::filesystem::path&) {
                return asset::upload_material(params, asset::MaterialTextures{}, allocator,
                                              transfer);
            });

        if (!mesh.is_loaded() && failures.find(mesh_key) == std::string::npos) {
            failures += (failures.empty() ? "" : ", ") + mesh_key;
        }
        if (!material.is_loaded() && failures.find(material_key) == std::string::npos) {
            failures += (failures.empty() ? "" : ", ") + material_key;
        }
        registry.emplace_or_replace<MeshRenderer>(e, std::move(mesh), std::move(material));
        // Recorded even on failure: the AssetManager caches the failed slot,
        // so retrying every frame could not succeed.
        p.resolved = primitive_hash(p);
    }
    if (!failures.empty()) {
        return std::unexpected(core::Error{"primitive asset upload failed: " + failures});
    }
    return stale.size();
}

} // namespace engine::scene
