// glTF images: decode (asset::decode_png_jpeg for PNG/JPEG,
// asset::decode_ktx2 for KTX2 / KHR_texture_basisu) on worker threads under
// one budget per load, and texture upload.

#include "engine/scene/gltf_loader.hpp"

#include <algorithm>
#include <atomic>
#include <string>
#include <thread>
#include <utility>
#include <vector>

#include <fastgltf/core.hpp>
#include <fastgltf/types.hpp>

#include "engine/asset/alpha_coverage.hpp"
#include "engine/asset/image_decode.hpp"
#include "engine/asset/ktx2_texture.hpp"
#include "scene/gltf/internal.hpp"

namespace engine::scene {

namespace gltf_detail {

std::expected<std::vector<TextureData>, core::Error>
extract_textures(const fastgltf::Asset& asset, const rhi::TextureCaps& caps,
                 const asset::TextureLimits& limits) {
    const std::vector<ImageUsage> uses = image_usages(asset);
    const std::size_t n = asset.images.size();
    std::vector<std::expected<TextureData, core::Error>> results(n);
    asset::DecodeBudget budget(limits.max_total_decoded_bytes);

    // Decoding / transcoding is independent per image: spread it over worker
    // threads, keeping the calling thread free of per-image work. Every decode
    // claims its output from `budget` before allocating it.
    std::atomic<std::size_t> next{0};
    const auto work = [&] {
        for (std::size_t i = next.fetch_add(1); i < n; i = next.fetch_add(1)) {
            if (uses[i].fallback && !uses[i].sampled) continue; // never sampled here
            auto encoded = data_source_bytes(asset, asset.images[i].data);
            if (!encoded) {
                results[i] = std::unexpected(encoded.error());
                continue;
            }
            ImageDecodeOptions options;
            options.color_space = uses[i].color_space;
            options.role = uses[i].role;
            options.caps = caps;
            options.limits = limits;
            options.budget = &budget;
            results[i] = decode_image(*encoded, options);
            // Alpha-tested base colour decoded to one RGBA8 image: build its
            // coverage-preserving mips here instead of blitting on upload
            // (KTX2 chains were built that way by the cooker). Over budget it
            // keeps the blitted mips: thinner at a distance, still correct.
            if (results[i] && uses[i].alpha_test_cutoff.has_value()) {
                (void)asset::apply_alpha_tested_mips(*results[i], *uses[i].alpha_test_cutoff,
                                                     &budget, limits.max_decoded_bytes);
            }
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

std::expected<TextureData, core::Error> decode_image(std::span<const std::byte> encoded,
                                                     const ImageDecodeOptions& options) {
    const asset::TextureLimits limits = asset::clamp_to_device(options.limits, options.caps);
    if (asset::is_ktx2(encoded)) {
        asset::Ktx2DecodeOptions ktx;
        ktx.caps = options.caps;
        ktx.role = options.role;
        ktx.color_space = options.color_space;
        ktx.limits = limits;
        ktx.budget = options.budget;
        return asset::decode_ktx2(encoded, ktx);
    }
    return asset::decode_png_jpeg(encoded, options.color_space, limits, options.budget);
}

std::expected<TextureData, core::Error> decode_image(std::span<const std::byte> encoded,
                                                     asset::TextureColorSpace color_space) {
    ImageDecodeOptions options;
    options.color_space = color_space;
    return decode_image(encoded, options);
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
    auto data = extract_textures(*asset, transfer.texture_caps);
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
