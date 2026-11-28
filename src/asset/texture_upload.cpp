// TextureData -> GPU TextureAsset (any TexelFormat, generated or cooked mips).

#include "engine/asset/texture_asset.hpp"

#include <cstring>
#include <string>
#include <utility>
#include <vector>

namespace engine::asset {

namespace {

[[nodiscard]] std::unexpected<core::Error> fail(std::string what) {
    return std::unexpected(core::Error{std::move(what)});
}

[[nodiscard]] VkComponentMapping component_mapping(TextureSwizzle swizzle) noexcept {
    switch (swizzle) {
        case TextureSwizzle::identity: break;
        case TextureSwizzle::rrr1:
            return {VK_COMPONENT_SWIZZLE_R, VK_COMPONENT_SWIZZLE_R, VK_COMPONENT_SWIZZLE_R,
                    VK_COMPONENT_SWIZZLE_ONE};
        case TextureSwizzle::ra01:
            return {VK_COMPONENT_SWIZZLE_R, VK_COMPONENT_SWIZZLE_A, VK_COMPONENT_SWIZZLE_ZERO,
                    VK_COMPONENT_SWIZZLE_ONE};
    }
    return {};
}

} // namespace

VkFormat texel_vk_format(TexelFormat format, TextureColorSpace color_space) noexcept {
    const bool srgb = color_space == TextureColorSpace::srgb;
    switch (format) {
        case TexelFormat::rgba8:
            return srgb ? VK_FORMAT_R8G8B8A8_SRGB : VK_FORMAT_R8G8B8A8_UNORM;
        case TexelFormat::bc7:
            return srgb ? VK_FORMAT_BC7_SRGB_BLOCK : VK_FORMAT_BC7_UNORM_BLOCK;
        case TexelFormat::bc5: return VK_FORMAT_BC5_UNORM_BLOCK;
        case TexelFormat::bc4: return VK_FORMAT_BC4_UNORM_BLOCK;
        case TexelFormat::astc_4x4:
            return srgb ? VK_FORMAT_ASTC_4x4_SRGB_BLOCK : VK_FORMAT_ASTC_4x4_UNORM_BLOCK;
    }
    return VK_FORMAT_UNDEFINED;
}

std::expected<TextureAsset, core::Error>
upload_texture_data(const TextureData& data, rhi::GpuAllocator& allocator,
                    const rhi::TransferContext& transfer) {
    if (data.width == 0 || data.height == 0 || data.pixels.empty()) {
        return fail("upload_texture: empty texture data");
    }
    const VkFormat format = texel_vk_format(data.format, data.color_space);
    const VkExtent2D extent{data.width, data.height};

    std::expected<rhi::GpuImage, core::Error> image = std::unexpected(core::Error{});
    std::uint32_t mips = 1;
    if (data.levels.empty()) {
        if (data.format != TexelFormat::rgba8) {
            return fail("upload_texture: compressed data needs its mip levels listed");
        }
        if (data.pixels.size() != texel_level_size(data.format, data.width, data.height)) {
            return fail("upload_texture: pixel data does not match the extent");
        }
        mips = rhi::full_mip_levels(extent);
        image = rhi::upload_device_image(allocator, transfer, data.pixels.data(),
                                         data.pixels.size(), extent, format,
                                         VK_IMAGE_USAGE_SAMPLED_BIT, mips);
    } else {
        std::vector<rhi::ImageLevelRegion> regions;
        regions.reserve(data.levels.size());
        for (const TextureLevel& l : data.levels) {
            if (l.size != texel_level_size(data.format, l.width, l.height)) {
                return fail("upload_texture: level size does not match its extent");
            }
            regions.push_back({l.offset, l.size, l.width, l.height});
        }
        mips = static_cast<std::uint32_t>(regions.size());
        image = rhi::upload_device_image_levels(allocator, transfer, data.pixels.data(),
                                                data.pixels.size(), extent, format, regions);
    }
    if (!image) return std::unexpected(image.error());

    auto view = rhi::create_image_view(transfer.device, image->handle(), format,
                                       VK_IMAGE_ASPECT_COLOR_BIT, mips, VK_IMAGE_VIEW_TYPE_2D,
                                       0, 0, 1, component_mapping(data.swizzle));
    if (!view) return std::unexpected(view.error());

    auto sampler = rhi::create_sampler(transfer.device, rhi::SamplerAddress::repeat, mips,
                                       transfer.max_anisotropy);
    if (!sampler) return std::unexpected(sampler.error());

    TextureAsset texture;
    texture.image = std::move(*image);
    texture.view = std::move(*view);
    texture.sampler = std::move(*sampler);
    texture.width = data.width;
    texture.height = data.height;
    texture.format = format;
    return texture;
}

std::expected<TextureArray, core::Error>
upload_texture_array(std::span<const TextureData* const> layers, rhi::GpuAllocator& allocator,
                     const rhi::TransferContext& transfer) {
    if (layers.empty()) return fail("upload_texture_array: no layers");
    const TextureData& first = *layers[0];
    if (first.levels.empty()) return fail("upload_texture_array: layers need their mip levels");
    std::uint64_t total = 0;
    for (const TextureData* t : layers) {
        if (t->width != first.width || t->height != first.height || t->format != first.format ||
            t->color_space != first.color_space || t->swizzle != first.swizzle ||
            t->levels.size() != first.levels.size()) {
            return fail("upload_texture_array: layers differ in extent, format or mip count");
        }
        for (const TextureLevel& l : t->levels) {
            if (l.size != texel_level_size(t->format, l.width, l.height) || l.offset > t->pixels.size() ||
                l.size > t->pixels.size() - l.offset) {
                return fail("upload_texture_array: level size does not match its extent");
            }
            total += l.size;
        }
    }
    // Repack every layer's levels into one blob, layer-major.
    PixelBuffer blob = PixelBuffer::allocate(static_cast<std::size_t>(total));
    if (blob.empty()) return fail("upload_texture_array: out of memory");
    std::vector<rhi::ImageLevelRegion> regions;
    regions.reserve(layers.size() * first.levels.size());
    std::uint64_t offset = 0;
    for (const TextureData* t : layers) {
        for (const TextureLevel& l : t->levels) {
            std::memcpy(blob.data() + offset, t->pixels.data() + l.offset, l.size);
            regions.push_back({offset, l.size, l.width, l.height});
            offset += l.size;
        }
    }
    const VkFormat format = texel_vk_format(first.format, first.color_space);
    const auto mips = static_cast<std::uint32_t>(first.levels.size());
    const auto count = static_cast<std::uint32_t>(layers.size());
    auto image = rhi::upload_device_image_array_levels(allocator, transfer, blob.data(), blob.size(),
                                                       {first.width, first.height}, format, regions,
                                                       count);
    if (!image) return std::unexpected(image.error());
    auto view = rhi::create_image_view(transfer.device, image->handle(), format,
                                       VK_IMAGE_ASPECT_COLOR_BIT, mips,
                                       VK_IMAGE_VIEW_TYPE_2D_ARRAY, 0, 0, count,
                                       component_mapping(first.swizzle));
    if (!view) return std::unexpected(view.error());
    TextureArray out;
    out.image = std::move(*image);
    out.view = std::move(*view);
    out.layers = count;
    out.mip_levels = mips;
    out.format = format;
    return out;
}

} // namespace engine::asset
