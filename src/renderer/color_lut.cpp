#include "engine/renderer/color_lut.hpp"

#include <cstddef>
#include <string>
#include <utility>

#include <glm/gtc/packing.hpp>

namespace engine::renderer {

std::vector<std::uint16_t> color_lut_strip(const asset::CubeLut& lut) {
    const std::uint32_t n = lut.size;
    std::vector<std::uint16_t> texels(static_cast<std::size_t>(n) * n * n * 4);
    const std::uint16_t one = glm::packHalf1x16(1.0F);
    for (std::uint32_t b = 0; b < n; ++b) {
        for (std::uint32_t g = 0; g < n; ++g) {
            for (std::uint32_t r = 0; r < n; ++r) {
                const std::array<float, 3> c = lut.at(r, g, b);
                // Strip texel (x = b * n + r, y = g), row width n * n.
                const std::size_t i = (static_cast<std::size_t>(g) * n * n + b * n + r) * 4;
                for (std::size_t k = 0; k < 3; ++k) texels[i + k] = glm::packHalf1x16(c[k]);
                texels[i + 3] = one;
            }
        }
    }
    return texels;
}

std::expected<void, core::Error> check_color_lut_fits(std::uint32_t size,
                                                     const rhi::TextureCaps& caps) {
    // Vulkan guarantees maxImageDimension2D >= 4096 only; a 65^3 LUT's strip
    // is 4225 texels wide. 0 = limit unknown (tests without a device).
    const std::uint64_t width = static_cast<std::uint64_t>(size) * size;
    if (caps.max_dimension_2d != 0 && width > caps.max_dimension_2d) {
        return std::unexpected(core::Error{
            "color LUT: a " + std::to_string(size) + "^3 table needs a " + std::to_string(width) +
            "-texel-wide strip, but this device's maxImageDimension2D is " +
            std::to_string(caps.max_dimension_2d) + " (use a LUT of size " +
            std::to_string(max_color_lut_size(caps)) + " or less)"});
    }
    return {};
}

std::uint32_t max_color_lut_size(const rhi::TextureCaps& caps) {
    std::uint32_t n = asset::CubeLut::k_max_size;
    if (caps.max_dimension_2d == 0) return n;
    while (n > asset::CubeLut::k_min_size && static_cast<std::uint64_t>(n) * n > caps.max_dimension_2d) {
        --n;
    }
    return n;
}

std::expected<ColorLutTexture, core::Error>
ColorLutTexture::create(rhi::GpuAllocator& allocator, const rhi::TransferContext& transfer,
                        const asset::CubeLut& lut) {
    if (lut.size < asset::CubeLut::k_min_size || lut.size > asset::CubeLut::k_max_size ||
        lut.rgb.size() != static_cast<std::size_t>(lut.size) * lut.size * lut.size * 3) {
        return std::unexpected(core::Error{"color LUT: malformed table"});
    }
    if (auto fits = check_color_lut_fits(lut.size, transfer.texture_caps); !fits) {
        return std::unexpected(fits.error());
    }
    const std::vector<std::uint16_t> texels = color_lut_strip(lut);
    const VkExtent2D extent{lut.size * lut.size, lut.size};
    auto image = rhi::upload_device_image(allocator, transfer, texels.data(),
                                          texels.size() * sizeof(std::uint16_t), extent,
                                          color_lut_format);
    if (!image) return std::unexpected(image.error());
    auto view = rhi::create_image_view(transfer.device, image->handle(), color_lut_format);
    if (!view) return std::unexpected(view.error());
    ColorLutTexture out;
    out.image_ = std::move(*image);
    out.view_ = std::move(*view);
    out.size_ = lut.size;
    out.domain_min_ = lut.domain_min;
    out.domain_max_ = lut.domain_max;
    return out;
}

} // namespace engine::renderer
