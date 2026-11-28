#pragma once

// sRGB / UNORM format pairs. An sRGB format and its UNORM twin store the
// same bytes; only the view format decides whether sampling decodes and
// rendering (blending included) encodes them. Passes that must touch the
// stored bytes as they are — the tonemap writing an already encoded value,
// the editor UI blending in gamma space — use the UNORM twin through a
// mutable-format image view or a size-compatible copy.

#include <vulkan/vulkan.h>

namespace engine::rhi {

// The UNORM format storing the same bytes as sRGB `format`;
// VK_FORMAT_UNDEFINED when `format` is not an sRGB format.
[[nodiscard]] constexpr VkFormat unorm_of_srgb(VkFormat format) {
    switch (format) {
    case VK_FORMAT_R8_SRGB: return VK_FORMAT_R8_UNORM;
    case VK_FORMAT_R8G8_SRGB: return VK_FORMAT_R8G8_UNORM;
    case VK_FORMAT_R8G8B8_SRGB: return VK_FORMAT_R8G8B8_UNORM;
    case VK_FORMAT_B8G8R8_SRGB: return VK_FORMAT_B8G8R8_UNORM;
    case VK_FORMAT_R8G8B8A8_SRGB: return VK_FORMAT_R8G8B8A8_UNORM;
    case VK_FORMAT_B8G8R8A8_SRGB: return VK_FORMAT_B8G8R8A8_UNORM;
    case VK_FORMAT_A8B8G8R8_SRGB_PACK32: return VK_FORMAT_A8B8G8R8_UNORM_PACK32;
    case VK_FORMAT_BC1_RGB_SRGB_BLOCK: return VK_FORMAT_BC1_RGB_UNORM_BLOCK;
    case VK_FORMAT_BC1_RGBA_SRGB_BLOCK: return VK_FORMAT_BC1_RGBA_UNORM_BLOCK;
    case VK_FORMAT_BC2_SRGB_BLOCK: return VK_FORMAT_BC2_UNORM_BLOCK;
    case VK_FORMAT_BC3_SRGB_BLOCK: return VK_FORMAT_BC3_UNORM_BLOCK;
    case VK_FORMAT_BC7_SRGB_BLOCK: return VK_FORMAT_BC7_UNORM_BLOCK;
    case VK_FORMAT_ASTC_4x4_SRGB_BLOCK: return VK_FORMAT_ASTC_4x4_UNORM_BLOCK;
    default: return VK_FORMAT_UNDEFINED;
    }
}

[[nodiscard]] constexpr bool is_srgb(VkFormat format) {
    return unorm_of_srgb(format) != VK_FORMAT_UNDEFINED;
}

// `format` itself when it is not sRGB, else its UNORM twin: the format whose
// view reads and writes the stored bytes without conversion.
[[nodiscard]] constexpr VkFormat storage_unorm(VkFormat format) {
    const VkFormat unorm = unorm_of_srgb(format);
    return unorm != VK_FORMAT_UNDEFINED ? unorm : format;
}

} // namespace engine::rhi
