#pragma once

// TextureAsset — a GPU-resident, sampleable 2D texture (Phase 3, Slice 3.3).
//
// Owns its VkImage (VMA), VkImageView and VkSampler as move-only RAII wrappers,
// so a default-constructed TextureAsset is the empty fallback returned for an
// unloaded handle. The image is left in SHADER_READ_ONLY_OPTIMAL after upload.

#include <cstdint>
#include <expected>

#include <vulkan/vulkan.h>

#include "engine/asset/texture_data.hpp"
#include "engine/core/error.hpp"
#include "engine/rhi/gpu_allocator.hpp"

namespace engine::asset {

struct TextureAsset {
    rhi::GpuImage  image;
    rhi::ImageView view;
    rhi::Sampler   sampler;
    std::uint32_t  width  = 0;
    std::uint32_t  height = 0;
    VkFormat       format = VK_FORMAT_UNDEFINED;

    [[nodiscard]] bool valid() const noexcept {
        return image.handle() != VK_NULL_HANDLE && view.handle() != VK_NULL_HANDLE
               && sampler.handle() != VK_NULL_HANDLE;
    }
};

// Vulkan format for `format` in `color_space` (bc5/bc4 have no sRGB variant
// and are always UNORM).
[[nodiscard]] VkFormat texel_vk_format(TexelFormat format, TextureColorSpace color_space) noexcept;

// Uploads `data` to a device-local image with its view (swizzled as
// requested) and a repeat sampler. RGBA8 without levels gets its mip chain
// generated; otherwise the given levels are uploaded as they are. The device
// must support the format (see rhi::TextureCaps).
[[nodiscard]] std::expected<TextureAsset, core::Error>
upload_texture_data(const TextureData& data, rhi::GpuAllocator& allocator,
                    const rhi::TransferContext& transfer);

} // namespace engine::asset
