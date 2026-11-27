// glTF images: decode (stb_image for PNG/JPEG, asset::decode_ktx2 for KTX2 /
// KHR_texture_basisu) on worker threads, and texture upload.

#include "engine/scene/gltf_loader.hpp"

#include <algorithm>
#include <atomic>
#include <limits>
#include <cstring>
#include <string>
#include <thread>
#include <utility>
#include <variant>
#include <vector>

#include <fastgltf/core.hpp>
#include <fastgltf/tools.hpp>
#include <fastgltf/types.hpp>

#include "engine/asset/ktx2_texture.hpp"
#include "scene/gltf/internal.hpp"

// Declarations only — the stb_image implementation lives in stb_image_impl.cpp,
// compiled permissively (see CMakeLists.txt), so its warnings stay out of our
// strict TUs.
#include <stb_image.h>

namespace engine::scene {

namespace gltf_detail {

namespace {

using ByteSpan = std::expected<std::span<const std::byte>, core::Error>;

template <typename Container>
[[nodiscard]] std::span<const std::byte> as_byte_span(const Container& c) {
    return {c.data(), c.size()};
}

// Largest PNG/JPEG edge accepted from a file (matches asset::Ktx2Limits).
constexpr int k_max_image_dimension = 16384;

// How a material reads an image; decides colour space and transcode role.
struct ImageUse {
    bool srgb = false;    // base colour / emissive
    bool normal = false;  // normal map
    bool single = false;  // occlusion (R only)
    bool other = false;   // linear multi-channel data (metallic-roughness, thickness)
    bool sampled = false; // the image some texture samples
    bool fallback = false; // a KHR_texture_basisu texture's core `source`
};

[[nodiscard]] std::vector<ImageUse> image_uses(const fastgltf::Asset& asset) {
    std::vector<ImageUse> uses(asset.images.size());
    for (const fastgltf::Texture& tex : asset.textures) {
        if (const auto img = texture_image(tex); img && *img < uses.size()) {
            uses[*img].sampled = true;
        }
        if (tex.basisuImageIndex.has_value() && tex.imageIndex.has_value() &&
            tex.imageIndex.value() < uses.size()) {
            uses[tex.imageIndex.value()].fallback = true;
        }
    }
    const auto mark = [&](const auto& info, bool ImageUse::* flag) {
        if (!info.has_value() || info->textureIndex >= asset.textures.size()) return;
        if (const auto img = texture_image(asset.textures[info->textureIndex]);
            img && *img < uses.size()) {
            uses[*img].*flag = true;
        }
    };
    for (const fastgltf::Material& mat : asset.materials) {
        mark(mat.pbrData.baseColorTexture, &ImageUse::srgb);
        mark(mat.emissiveTexture, &ImageUse::srgb);
        mark(mat.normalTexture, &ImageUse::normal);
        mark(mat.occlusionTexture, &ImageUse::single);
        mark(mat.pbrData.metallicRoughnessTexture, &ImageUse::other);
        if (mat.volume != nullptr) mark(mat.volume->thicknessTexture, &ImageUse::other);
    }
    return uses;
}

[[nodiscard]] asset::TextureRole role_of(const ImageUse& u) {
    if (u.srgb || u.other) return asset::TextureRole::color;
    if (u.normal && !u.single) return asset::TextureRole::normal;
    if (u.single && !u.normal) return asset::TextureRole::single_channel;
    return asset::TextureRole::color;
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
                if (view.bufferIndex >= asset.buffers.size()) {
                    return fail("glTF image bufferView buffer index out of range");
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

std::expected<std::vector<TextureData>, core::Error>
extract_textures(const fastgltf::Asset& asset, const rhi::TextureCompressionSupport& caps) {
    const std::vector<ImageUse> uses = image_uses(asset);
    const std::size_t n = asset.images.size();
    std::vector<std::expected<TextureData, core::Error>> results(n, TextureData{});

    // Decoding / transcoding is independent per image: spread it over worker
    // threads, keeping the calling thread free of per-image work.
    std::atomic<std::size_t> next{0};
    const auto work = [&] {
        for (std::size_t i = next.fetch_add(1); i < n; i = next.fetch_add(1)) {
            if (uses[i].fallback && !uses[i].sampled) continue; // never sampled here
            auto encoded = data_source_bytes(asset, asset.images[i].data);
            if (!encoded) {
                results[i] = std::unexpected(encoded.error());
                continue;
            }
            const auto space = uses[i].srgb ? asset::TextureColorSpace::srgb
                                            : asset::TextureColorSpace::linear;
            results[i] = decode_image(*encoded, space, role_of(uses[i]), caps);
        }
    };
    const std::size_t workers =
        std::min<std::size_t>(n, std::clamp<std::size_t>(std::thread::hardware_concurrency(), 1, 8));
    std::vector<std::thread> threads;
    threads.reserve(workers);
    for (std::size_t t = 0; t < workers; ++t) threads.emplace_back(work);
    for (std::thread& t : threads) t.join();

    std::vector<TextureData> out;
    out.reserve(n);
    for (std::size_t i = 0; i < n; ++i) {
        if (!results[i]) {
            return fail("glTF image " + std::to_string(i) + ": " + results[i].error().message);
        }
        out.push_back(std::move(*results[i]));
    }
    return out;
}

[[nodiscard]] std::expected<std::vector<TextureData>, core::Error>
parse_and_extract_textures(fastgltf::GltfDataBuffer& data,
                           const std::filesystem::path& base_dir) {
    auto asset = parse_gltf(data, base_dir);
    if (!asset) return std::unexpected(asset.error());
    return extract_textures(*asset);
}

} // namespace gltf_detail

using namespace gltf_detail;

std::expected<TextureData, core::Error>
decode_image(std::span<const std::byte> encoded, asset::TextureColorSpace color_space,
             asset::TextureRole role, const rhi::TextureCompressionSupport& caps) {
    if (asset::is_ktx2(encoded)) {
        asset::Ktx2DecodeOptions options;
        options.caps = caps;
        options.role = role;
        options.color_space = color_space;
        return asset::decode_ktx2(encoded, options);
    }
    if (encoded.size() > static_cast<std::size_t>(std::numeric_limits<int>::max())) {
        return fail("stb_image: encoded image too large");
    }
    const auto* bytes = reinterpret_cast<const stbi_uc*>(encoded.data());
    const int len = static_cast<int>(encoded.size());
    int w = 0;
    int h = 0;
    int channels = 0;
    if (stbi_info_from_memory(bytes, len, &w, &h, &channels) == 0) {
        return fail("stb_image: unrecognised or corrupt image");
    }
    if (w <= 0 || h <= 0 || w > k_max_image_dimension || h > k_max_image_dimension) {
        return fail("stb_image: image dimensions out of range");
    }
    stbi_uc* pixels = stbi_load_from_memory(bytes, len, &w, &h, &channels, STBI_rgb_alpha);
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

std::expected<asset::TextureAsset, core::Error>
upload_texture(const TextureData& data, rhi::GpuAllocator& allocator,
               const rhi::TransferContext& transfer) {
    return asset::upload_texture_data(data, allocator, transfer);
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
    auto file = fastgltf::GltfDataBuffer::FromPath(path);
    if (file.error() != fastgltf::Error::None) {
        return fail("fastgltf: could not read '" + path.string() + "': " +
                    std::string(fastgltf::getErrorMessage(file.error())));
    }
    auto asset = parse_gltf(file.get(), path.parent_path());
    if (!asset) return std::unexpected(asset.error());
    auto data = extract_textures(*asset, transfer.compression);
    if (!data) return std::unexpected(data.error());

    std::vector<asset::TextureAsset> textures;
    textures.reserve(data->size());
    for (const TextureData& td : *data) {
        if (td.pixels.empty()) { // KHR_texture_basisu fallback image: not sampled
            textures.emplace_back();
            continue;
        }
        auto texture = upload_texture(td, allocator, transfer);
        if (!texture) return std::unexpected(texture.error());
        textures.push_back(std::move(*texture));
    }
    return textures;
}

} // namespace engine::scene
