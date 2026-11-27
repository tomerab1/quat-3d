// glTF PBR materials.

#define GLM_ENABLE_EXPERIMENTAL

#include "engine/scene/gltf_loader.hpp"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <functional>
#include <limits>
#include <string>
#include <string_view>
#include <unordered_map>
#include <utility>
#include <variant>
#include <vector>

#include <fastgltf/core.hpp>
#include <fastgltf/math.hpp>
#include <fastgltf/tools.hpp>
#include <fastgltf/types.hpp>
#include <glm/glm.hpp>
#include <glm/gtc/matrix_transform.hpp>
#include <glm/gtc/quaternion.hpp>
#include <glm/gtx/matrix_decompose.hpp>

#include "engine/animation/clip.hpp"
#include "engine/animation/skeleton.hpp"
#include "engine/asset/asset_manager.hpp"
#include "engine/scene/components.hpp"
#include "engine/scene/scene.hpp"
#include "scene/gltf/internal.hpp"

namespace engine::scene {

namespace gltf_detail {

// -------------------------------------------------------------------------
// Materials
// -------------------------------------------------------------------------

namespace {
// glTF image index behind a texture reference, or no_texture when unset. A
// texture with no image source also resolves to no_texture. Templated on the
// whole optional type: fastgltf::Optional is an alias (conditional_t), so the
// contained TextureInfo type can't be deduced through it directly.
template <typename OptTextureInfo>
[[nodiscard]] std::size_t image_of(const fastgltf::Asset& asset,
                                   const OptTextureInfo& info) {
    if (!info.has_value()) return no_texture;
    return texture_image(asset.textures[info->textureIndex]).value_or(no_texture);
}
} // namespace

namespace {
// Folds a texture slot's KHR_texture_transform into the per-slot UV arrays of
// `p` (see PbrMaterialParams: uv' = M * uv + offset, rows packed in uv_mat,
// offsets two per vec4). Templated like image_of: fastgltf::Optional hides the
// contained TextureInfo type.
template <typename OptTextureInfo>
void apply_uv_transform(asset::PbrMaterialParams& p, std::size_t slot,
                        const OptTextureInfo& info) {
    if (!info.has_value() || info->transform == nullptr) return;
    const fastgltf::TextureTransform& t = *info->transform;
    // T * R * S from the KHR_texture_transform spec.
    const float c = std::cos(t.rotation);
    const float s = std::sin(t.rotation);
    p.uv_mat[slot] = {c * t.uvScale[0], -s * t.uvScale[1],
                      s * t.uvScale[0],  c * t.uvScale[1]};
    glm::vec4& packed = p.uv_offset[slot / 2];
    if (slot % 2 == 0) {
        packed.x = t.uvOffset[0];
        packed.y = t.uvOffset[1];
    } else {
        packed.z = t.uvOffset[0];
        packed.w = t.uvOffset[1];
    }
}
} // namespace

[[nodiscard]] std::vector<MaterialData> extract_materials(const fastgltf::Asset& asset) {
    std::vector<MaterialData> out;
    out.reserve(asset.materials.size());
    for (const fastgltf::Material& mat : asset.materials) {
        MaterialData md;
        asset::PbrMaterialParams& p = md.params;

        const auto& bcf = mat.pbrData.baseColorFactor;
        p.base_color_factor = {bcf[0], bcf[1], bcf[2], bcf[3]};
        // KHR_materials_emissive_strength is a plain multiplier (1 when absent)
        // — premultiply it so no shader change is needed.
        const auto& ef = mat.emissiveFactor;
        p.emissive_factor = {ef[0] * mat.emissiveStrength, ef[1] * mat.emissiveStrength,
                             ef[2] * mat.emissiveStrength, 0.0F};
        p.metallic_factor = mat.pbrData.metallicFactor;
        p.roughness_factor = mat.pbrData.roughnessFactor;
        p.alpha_cutoff = mat.alphaCutoff;
        if (mat.normalTexture.has_value()) p.normal_scale = mat.normalTexture->scale;
        if (mat.occlusionTexture.has_value()) {
            p.occlusion_strength = mat.occlusionTexture->strength;
        }

        md.base_color_image = image_of(asset, mat.pbrData.baseColorTexture);
        md.metallic_roughness_image = image_of(asset, mat.pbrData.metallicRoughnessTexture);
        md.normal_image = image_of(asset, mat.normalTexture);
        md.occlusion_image = image_of(asset, mat.occlusionTexture);
        md.emissive_image = image_of(asset, mat.emissiveTexture);

        // KHR_texture_transform per slot (identity when absent).
        apply_uv_transform(p, asset::material_slot_base_color, mat.pbrData.baseColorTexture);
        apply_uv_transform(p, asset::material_slot_metallic_roughness,
                           mat.pbrData.metallicRoughnessTexture);
        apply_uv_transform(p, asset::material_slot_normal, mat.normalTexture);
        apply_uv_transform(p, asset::material_slot_occlusion, mat.occlusionTexture);
        apply_uv_transform(p, asset::material_slot_emissive, mat.emissiveTexture);

        std::uint32_t flags = 0;
        if (md.base_color_image != no_texture) flags |= asset::material_has_base_color;
        if (md.normal_image != no_texture) flags |= asset::material_has_normal;
        if (md.metallic_roughness_image != no_texture) {
            flags |= asset::material_has_metallic_roughness;
        }
        if (md.emissive_image != no_texture) flags |= asset::material_has_emissive;
        if (md.occlusion_image != no_texture) flags |= asset::material_has_occlusion;
        if (mat.alphaMode == fastgltf::AlphaMode::Blend) flags |= asset::material_blend;
        if (mat.alphaMode == fastgltf::AlphaMode::Mask) flags |= asset::material_alpha_mask;
        if (mat.doubleSided) flags |= asset::material_double_sided;
        // KHR_materials_transmission → see-through glass.
        if (mat.transmission != nullptr && mat.transmission->transmissionFactor > 0.0F) {
            flags |= asset::material_transmission;
            p.transmission_factor = mat.transmission->transmissionFactor;
        }
        // KHR_materials_ior (defaults to 1.5 when the extension is absent).
        p.ior = mat.ior;
        // KHR_materials_clearcoat — factors only; the coat strength/roughness/
        // normal textures are not yet bound (rarely authored, later slice).
        if (mat.clearcoat != nullptr) {
            p.clearcoat_factor = mat.clearcoat->clearcoatFactor;
            p.clearcoat_roughness = mat.clearcoat->clearcoatRoughnessFactor;
        }
        // KHR_materials_volume → Beer-Lambert attenuation through the medium.
        // The spec default attenuationDistance is +inf (no attenuation); store
        // that as 0 so the shader can branch on `attenuation.w > 0`.
        if (mat.volume != nullptr) {
            const auto& ac = mat.volume->attenuationColor;
            const float ad = mat.volume->attenuationDistance;
            p.attenuation = {ac[0], ac[1], ac[2], std::isfinite(ad) ? ad : 0.0F};
            p.thickness = mat.volume->thicknessFactor;
            md.thickness_image = image_of(asset, mat.volume->thicknessTexture);
            if (md.thickness_image != no_texture) flags |= asset::material_has_thickness;
            apply_uv_transform(p, asset::material_slot_thickness, mat.volume->thicknessTexture);
        }
        p.flags = flags;

        out.push_back(md);
    }
    return out;
}

[[nodiscard]] std::expected<std::vector<MaterialData>, core::Error>
parse_and_extract_materials(fastgltf::GltfDataBuffer& data,
                            const std::filesystem::path& base_dir) {
    auto asset = parse_gltf(data, base_dir);
    if (!asset) return std::unexpected(asset.error());
    return extract_materials(*asset);
}


} // namespace gltf_detail

using namespace gltf_detail;

std::expected<std::vector<MaterialData>, core::Error>
GltfLoader::load_material_data(const std::filesystem::path& path) {
    auto data = fastgltf::GltfDataBuffer::FromPath(path);
    if (data.error() != fastgltf::Error::None) {
        return fail("fastgltf: could not read '" + path.string() + "': " +
                    std::string(fastgltf::getErrorMessage(data.error())));
    }
    return parse_and_extract_materials(data.get(), path.parent_path());
}

std::expected<std::vector<MaterialData>, core::Error>
GltfLoader::load_material_data_from_memory(std::span<const std::byte> bytes,
                                           const std::filesystem::path& base_dir) {
    auto data = fastgltf::GltfDataBuffer::FromBytes(bytes.data(), bytes.size());
    if (data.error() != fastgltf::Error::None) {
        return fail("fastgltf: could not wrap memory buffer: " +
                    std::string(fastgltf::getErrorMessage(data.error())));
    }
    return parse_and_extract_materials(data.get(), base_dir);
}

} // namespace engine::scene
