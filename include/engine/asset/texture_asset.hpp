#pragma once

// TextureAsset — a GPU-resident, sampleable 2D texture (Phase 3, Slice 3.3).
//
// Owns its VkImage (VMA), VkImageView and VkSampler as move-only RAII wrappers,
// so a default-constructed TextureAsset is the empty fallback returned for an
// unloaded handle. The image is left in SHADER_READ_ONLY_OPTIMAL after upload.

#include <cstdint>

#include <vulkan/vulkan.h>

#include "engine/rhi/gpu_allocator.hpp"

namespace engine::asset {

// How a glTF texture's bytes should be interpreted. Base colour and emissive are
// authored in sRGB; normal, metallic-roughness and occlusion are linear data.
enum class TextureColorSpace : std::uint8_t { srgb, linear };

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

} // namespace engine::asset
