// glTF textures (decode + upload) and PBR materials.

#define GLM_ENABLE_EXPERIMENTAL

#include "engine/scene/gltf_loader.hpp"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <functional>
#include <limits>
#include <optional>
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
#include "engine/asset/alpha_coverage.hpp"
#include "engine/asset/asset_manager.hpp"
#include "engine/scene/components.hpp"
#include "engine/scene/scene.hpp"
#include "scene/gltf/internal.hpp"

// Declarations only — the stb_image implementation lives in stb_image_impl.cpp,
// compiled permissively (see CMakeLists.txt), so its warnings stay out of our
// strict TUs.
#include <stb_image.h>

namespace engine::scene {

namespace gltf_detail {

namespace {
template <typename Container>
[[nodiscard]] std::span<const std::byte> as_byte_span(const Container& c) {
    return {c.data(), c.size()};
}
} // namespace

namespace {
// Resolve the encoded image/buffer bytes behind a fastgltf DataSource. After
// LoadExternalBuffers/LoadExternalImages, images resolve to sources::Array or a
// sources::BufferView into an already-loaded buffer.
[[nodiscard]] std::expected<std::span<const std::byte>, core::Error>
data_source_bytes(const fastgltf::Asset& asset, const fastgltf::DataSource& source) {
    return std::visit(
        fastgltf::visitor{
            [](const fastgltf::sources::Array& a)
                -> std::expected<std::span<const std::byte>, core::Error> {
                return as_byte_span(a.bytes);
            },
            [](const fastgltf::sources::Vector& v)
                -> std::expected<std::span<const std::byte>, core::Error> {
                return as_byte_span(v.bytes);
            },
            [](const fastgltf::sources::ByteView& b)
                -> std::expected<std::span<const std::byte>, core::Error> {
                return b.bytes;
            },
            [&](const fastgltf::sources::BufferView& bv)
                -> std::expected<std::span<const std::byte>, core::Error> {
                const fastgltf::BufferView& view = asset.bufferViews[bv.bufferViewIndex];
                auto buffer = data_source_bytes(asset, asset.buffers[view.bufferIndex].data);
                if (!buffer) return buffer;
                if (view.byteOffset + view.byteLength > buffer->size()) {
                    return fail("glTF image bufferView out of range");
                }
                return buffer->subspan(view.byteOffset, view.byteLength);
            },
            [](const auto&) -> std::expected<std::span<const std::byte>, core::Error> {
                return fail("glTF image has an unsupported / unloaded data source "
                            "(external file not resolved?)");
            }},
        source);
}
} // namespace

namespace {
// Per-image colour space: sRGB if any material references the image as a base
// colour or emissive texture, otherwise linear (normal, metallic-roughness, AO).
[[nodiscard]] std::vector<asset::TextureColorSpace>
resolve_color_spaces(const fastgltf::Asset& asset) {
    std::vector<asset::TextureColorSpace> spaces(asset.images.size(),
                                                 asset::TextureColorSpace::linear);
    const auto mark_srgb = [&](const fastgltf::Optional<fastgltf::TextureInfo>& info) {
        if (!info.has_value()) return;
        const fastgltf::Texture& tex = asset.textures[info->textureIndex];
        if (tex.imageIndex.has_value()) {
            spaces[tex.imageIndex.value()] = asset::TextureColorSpace::srgb;
        }
    };
    for (const fastgltf::Material& mat : asset.materials) {
        mark_srgb(mat.pbrData.baseColorTexture);
        mark_srgb(mat.emissiveTexture);
    }
    return spaces;
}

// Per-image alpha-test cutoff: the cutoff of the alphaMode MASK material
// sampling the image as its base colour (the lowest, if several do: the
// coverage that cutoff preserves is the largest), else none.
[[nodiscard]] std::vector<std::optional<float>>
resolve_alpha_test_cutoffs(const fastgltf::Asset& asset) {
    std::vector<std::optional<float>> cutoffs(asset.images.size());
    for (const fastgltf::Material& mat : asset.materials) {
        if (mat.alphaMode != fastgltf::AlphaMode::Mask) continue;
        const auto& info = mat.pbrData.baseColorTexture;
        if (!info.has_value()) continue;
        const fastgltf::Texture& tex = asset.textures[info->textureIndex];
        if (!tex.imageIndex.has_value()) continue;
        std::optional<float>& c = cutoffs[tex.imageIndex.value()];
        const float cutoff = std::clamp(mat.alphaCutoff, 0.0F, 1.0F);
        c = c.has_value() ? std::min(*c, cutoff) : cutoff;
    }
    return cutoffs;
}
} // namespace

[[nodiscard]] std::expected<std::vector<TextureData>, core::Error>
extract_textures(const fastgltf::Asset& asset) {
    const std::vector<asset::TextureColorSpace> spaces = resolve_color_spaces(asset);
    const std::vector<std::optional<float>> cutoffs = resolve_alpha_test_cutoffs(asset);
    std::vector<TextureData> out;
    out.reserve(asset.images.size());
    for (std::size_t i = 0; i < asset.images.size(); ++i) {
        auto encoded = data_source_bytes(asset, asset.images[i].data);
        if (!encoded) return std::unexpected(encoded.error());
        auto decoded = decode_image(*encoded, spaces[i]);
        if (!decoded) return std::unexpected(decoded.error());
        decoded->alpha_test_cutoff = cutoffs[i];
        out.push_back(std::move(*decoded));
    }
    return out;
}

[[nodiscard]] std::expected<std::vector<TextureData>, core::Error>
parse_and_extract_textures(fastgltf::GltfDataBuffer& data,
                           const std::filesystem::path& base_dir) {
    fastgltf::Parser parser(kParserExtensions);
    fastgltf::Expected<fastgltf::Asset> asset = parser.loadGltf(data, base_dir, kLoadOptions);
    if (asset.error() != fastgltf::Error::None) {
        return fail(std::string("fastgltf parse failed: ") +
                    std::string(fastgltf::getErrorMessage(asset.error())));
    }
    if (auto valid = validate_asset(asset.get()); !valid) return std::unexpected(valid.error());
    return extract_textures(asset.get());
}

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
    const fastgltf::Texture& tex = asset.textures[info->textureIndex];
    return tex.imageIndex.has_value() ? tex.imageIndex.value() : no_texture;
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
    fastgltf::Parser parser(kParserExtensions);
    fastgltf::Expected<fastgltf::Asset> asset = parser.loadGltf(data, base_dir, kLoadOptions);
    if (asset.error() != fastgltf::Error::None) {
        return fail(std::string("fastgltf parse failed: ") +
                    std::string(fastgltf::getErrorMessage(asset.error())));
    }
    if (auto valid = validate_asset(asset.get()); !valid) return std::unexpected(valid.error());
    return extract_materials(asset.get());
}


} // namespace gltf_detail

using namespace gltf_detail;

// ---------------------------------------------------------------------------
// Textures
// ---------------------------------------------------------------------------
std::expected<TextureData, core::Error>
decode_image(std::span<const std::byte> encoded, asset::TextureColorSpace color_space) {
    int w = 0;
    int h = 0;
    int channels = 0;
    stbi_uc* pixels = stbi_load_from_memory(
        reinterpret_cast<const stbi_uc*>(encoded.data()),
        static_cast<int>(encoded.size()), &w, &h, &channels, STBI_rgb_alpha);
    if (pixels == nullptr) {
        return fail("stb_image: failed to decode image");
    }

    TextureData out;
    out.width = static_cast<std::uint32_t>(w);
    out.height = static_cast<std::uint32_t>(h);
    out.color_space = color_space;
    const std::size_t byte_count = static_cast<std::size_t>(w) * static_cast<std::size_t>(h) * 4U;
    out.pixels.resize(byte_count);
    std::memcpy(out.pixels.data(), pixels, byte_count);
    stbi_image_free(pixels);
    return out;
}

namespace {
// An alpha-tested (MASK) base colour: the mip chain is filtered on the CPU
// with coverage-preserving alpha so cut-outs don't thin out with distance.
[[nodiscard]] std::expected<rhi::GpuImage, core::Error>
upload_alpha_tested(const TextureData& data, rhi::GpuAllocator& allocator,
                    const rhi::TransferContext& transfer, VkFormat format) {
    const std::span<const std::uint8_t> rgba(
        reinterpret_cast<const std::uint8_t*>(data.pixels.data()), data.pixels.size());
    const std::vector<asset::MipLevelRgba8> chain = asset::build_alpha_tested_mips(
        rgba, data.width, data.height, data.color_space == asset::TextureColorSpace::srgb,
        data.alpha_test_cutoff.value_or(0.5F));
    if (chain.empty()) return fail("upload_texture: pixel data does not match the extent");
    std::vector<rhi::MipLevelData> levels;
    levels.reserve(chain.size());
    for (const asset::MipLevelRgba8& l : chain) {
        levels.push_back({l.pixels.data(), l.pixels.size(), {l.width, l.height}});
    }
    return rhi::upload_device_mip_chain(allocator, transfer, levels, format);
}
} // namespace

std::expected<asset::TextureAsset, core::Error>
upload_texture(const TextureData& data, rhi::GpuAllocator& allocator,
               const rhi::TransferContext& transfer) {
    if (data.width == 0 || data.height == 0 || data.pixels.empty()) {
        return fail("upload_texture: empty texture data");
    }
    const VkFormat format = data.color_space == asset::TextureColorSpace::srgb
                                ? VK_FORMAT_R8G8B8A8_SRGB
                                : VK_FORMAT_R8G8B8A8_UNORM;
    const VkExtent2D extent{data.width, data.height};
    const std::uint32_t mips = rhi::full_mip_levels(extent);

    auto image = data.alpha_test_cutoff.has_value()
                     ? upload_alpha_tested(data, allocator, transfer, format)
                     : rhi::upload_device_image(allocator, transfer, data.pixels.data(),
                                                data.pixels.size(), extent, format,
                                                VK_IMAGE_USAGE_SAMPLED_BIT, mips);
    if (!image) return std::unexpected(image.error());

    auto view = rhi::create_image_view(transfer.device, image->handle(), format,
                                       VK_IMAGE_ASPECT_COLOR_BIT, mips);
    if (!view) return std::unexpected(view.error());

    auto sampler = rhi::create_sampler(transfer.device, rhi::SamplerAddress::repeat,
                                       mips, transfer.max_anisotropy);
    if (!sampler) return std::unexpected(sampler.error());

    asset::TextureAsset texture;
    texture.image = std::move(*image);
    texture.view = std::move(*view);
    texture.sampler = std::move(*sampler);
    texture.width = data.width;
    texture.height = data.height;
    texture.format = format;
    return texture;
}

std::expected<std::vector<TextureData>, core::Error>
GltfLoader::load_texture_data(const std::filesystem::path& path) {
    auto data = fastgltf::GltfDataBuffer::FromPath(path);
    if (data.error() != fastgltf::Error::None) {
        return fail("fastgltf: could not read '" + path.string() + "': " +
                    std::string(fastgltf::getErrorMessage(data.error())));
    }
    return parse_and_extract_textures(data.get(), path.parent_path());
}

std::expected<std::vector<TextureData>, core::Error>
GltfLoader::load_texture_data_from_memory(std::span<const std::byte> bytes,
                                          const std::filesystem::path& base_dir) {
    auto data = fastgltf::GltfDataBuffer::FromBytes(bytes.data(), bytes.size());
    if (data.error() != fastgltf::Error::None) {
        return fail("fastgltf: could not wrap memory buffer: " +
                    std::string(fastgltf::getErrorMessage(data.error())));
    }
    return parse_and_extract_textures(data.get(), base_dir);
}

std::expected<std::vector<asset::TextureAsset>, core::Error>
GltfLoader::load_textures(const std::filesystem::path& path, rhi::GpuAllocator& allocator,
                          const rhi::TransferContext& transfer) {
    auto data = load_texture_data(path);
    if (!data) return std::unexpected(data.error());

    std::vector<asset::TextureAsset> textures;
    textures.reserve(data->size());
    for (const TextureData& td : *data) {
        auto texture = upload_texture(td, allocator, transfer);
        if (!texture) return std::unexpected(texture.error());
        textures.push_back(std::move(*texture));
    }
    return textures;
}

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
