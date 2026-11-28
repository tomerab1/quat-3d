#pragma once

// ColorLutTexture — a `.cube` look LUT on the GPU (ROADMAP R20): a size^3
// RGBA16F VK_IMAGE_TYPE_3D image (x = red, y = green, z = blue), sampled with
// one hardware-trilinear lookup by lib/color.slang's sample_lut. Vulkan
// guarantees maxImageDimension3D >= 256, so every size the parser accepts
// (2..65) fits on every device.

#include <array>
#include <cstdint>
#include <expected>
#include <vector>

#include <vulkan/vulkan.h>

#include "engine/asset/cube_lut.hpp"
#include "engine/core/error.hpp"
#include "engine/rhi/gpu_allocator.hpp"

namespace engine::renderer {

inline constexpr VkFormat color_lut_format = VK_FORMAT_R16G16B16A16_SFLOAT;

// The image's texels (RGBA16F, red fastest, then green, then blue), as uploaded.
[[nodiscard]] std::vector<std::uint16_t> color_lut_texels(const asset::CubeLut& lut);

class ColorLutTexture {
public:
    // Uploads `lut` (already validated by asset::parse_cube_lut).
    [[nodiscard]] static std::expected<ColorLutTexture, core::Error>
    create(rhi::GpuAllocator& allocator, const rhi::TransferContext& transfer,
           const asset::CubeLut& lut);

    ColorLutTexture() = default;
    ColorLutTexture(ColorLutTexture&&) noexcept = default;
    ColorLutTexture& operator=(ColorLutTexture&&) noexcept = default;
    ColorLutTexture(const ColorLutTexture&) = delete;
    ColorLutTexture& operator=(const ColorLutTexture&) = delete;

    [[nodiscard]] VkImageView view() const { return view_.handle(); }
    [[nodiscard]] std::uint32_t size() const { return size_; }
    [[nodiscard]] const std::array<float, 3>& domain_min() const { return domain_min_; }
    [[nodiscard]] const std::array<float, 3>& domain_max() const { return domain_max_; }

private:
    rhi::GpuImage        image_;
    rhi::ImageView       view_;
    std::uint32_t        size_ = 0;
    std::array<float, 3> domain_min_{0.0F, 0.0F, 0.0F};
    std::array<float, 3> domain_max_{1.0F, 1.0F, 1.0F};
};

} // namespace engine::renderer
