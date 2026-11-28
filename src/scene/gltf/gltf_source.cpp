// glTF buffer / image data access and image usage (see gltf_io.hpp).

#include <algorithm>
#include <optional>
#include <variant>
#include <vector>

#include <fastgltf/tools.hpp>

#include "scene/gltf/gltf_io.hpp"

namespace engine::scene::gltf_detail {

namespace {

using ByteSpan = std::expected<std::span<const std::byte>, core::Error>;

template <typename Container>
[[nodiscard]] std::span<const std::byte> as_byte_span(const Container& c) {
    return {c.data(), c.size()};
}

} // namespace

std::optional<std::size_t> texture_image(const fastgltf::Texture& texture) {
    if (texture.basisuImageIndex.has_value()) return texture.basisuImageIndex.value();
    if (texture.imageIndex.has_value()) return texture.imageIndex.value();
    return std::nullopt;
}

// After LoadExternalBuffers/LoadExternalImages, sources resolve to loaded
// bytes (Array / Vector / ByteView), or for images a BufferView into one.
std::expected<std::span<const std::byte>, core::Error>
data_source_bytes(const fastgltf::Asset& asset, const fastgltf::DataSource& source) {
    return std::visit(
        fastgltf::visitor{
            [](const fastgltf::sources::Array& a) -> ByteSpan { return as_byte_span(a.bytes); },
            [](const fastgltf::sources::Vector& v) -> ByteSpan { return as_byte_span(v.bytes); },
            [](const fastgltf::sources::ByteView& b) -> ByteSpan { return b.bytes; },
            [&](const fastgltf::sources::BufferView& bv) -> ByteSpan {
                if (bv.bufferViewIndex >= asset.bufferViews.size()) {
                    return fail("glTF image bufferView index out of range");
                }
                const fastgltf::BufferView& view = asset.bufferViews[bv.bufferViewIndex];
                if (view.bufferIndex >= asset.buffers.size() ||
                    std::holds_alternative<fastgltf::sources::BufferView>(
                        asset.buffers[view.bufferIndex].data)) {
                    return fail("glTF image bufferView buffer index invalid");
                }
                auto buffer = data_source_bytes(asset, asset.buffers[view.bufferIndex].data);
                if (!buffer) return buffer;
                if (view.byteOffset > buffer->size() ||
                    view.byteLength > buffer->size() - view.byteOffset) {
                    return fail("glTF image bufferView out of range");
                }
                return buffer->subspan(view.byteOffset, view.byteLength);
            },
            [](const auto&) -> ByteSpan {
                return fail("glTF data source not loaded (external file not resolved, or an "
                            "EXT_meshopt_compression fallback buffer)");
            }},
        source);
}

std::vector<ImageUsage> image_usages(const fastgltf::Asset& asset) {
    const std::size_t n = asset.images.size();
    std::vector<ImageUsage> out(n);
    std::vector<std::uint8_t> srgb(n), normal(n), single(n), other(n);
    for (const fastgltf::Texture& tex : asset.textures) {
        if (const auto img = texture_image(tex); img && *img < n) out[*img].sampled = true;
        if (tex.basisuImageIndex.has_value() && tex.imageIndex.has_value() &&
            tex.imageIndex.value() < n) {
            out[tex.imageIndex.value()].fallback = true;
        }
    }
    const auto mark = [&](const auto& info, std::vector<std::uint8_t>& flag) {
        if (!info.has_value() || info->textureIndex >= asset.textures.size()) return;
        if (const auto img = texture_image(asset.textures[info->textureIndex]); img && *img < n) {
            flag[*img] = 1;
        }
    };
    for (const fastgltf::Material& mat : asset.materials) {
        mark(mat.pbrData.baseColorTexture, srgb);
        mark(mat.emissiveTexture, srgb);
        mark(mat.normalTexture, normal);
        mark(mat.occlusionTexture, single);
        mark(mat.pbrData.metallicRoughnessTexture, other);
        if (mat.volume != nullptr) mark(mat.volume->thicknessTexture, other);
        const auto& base = mat.pbrData.baseColorTexture;
        if (mat.alphaMode == fastgltf::AlphaMode::Mask && base.has_value() &&
            base->textureIndex < asset.textures.size()) {
            if (const auto img = texture_image(asset.textures[base->textureIndex]);
                img && *img < n) {
                std::optional<float>& c = out[*img].alpha_test_cutoff;
                const float cutoff = std::clamp(mat.alphaCutoff, 0.0F, 1.0F);
                c = c.has_value() ? std::min(*c, cutoff) : cutoff;
            }
        }
    }
    for (std::size_t i = 0; i < n; ++i) {
        if (srgb[i] != 0) out[i].color_space = asset::TextureColorSpace::srgb;
        if (srgb[i] == 0 && other[i] == 0) {
            if (normal[i] != 0 && single[i] == 0) out[i].role = asset::TextureRole::normal;
            if (single[i] != 0 && normal[i] == 0) out[i].role = asset::TextureRole::single_channel;
        }
    }
    return out;
}

} // namespace engine::scene::gltf_detail
