#include "engine/renderer/color_lut.hpp"

#include <cstddef>
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

std::expected<ColorLutTexture, core::Error>
ColorLutTexture::create(rhi::GpuAllocator& allocator, const rhi::TransferContext& transfer,
                        const asset::CubeLut& lut) {
    if (lut.size < asset::CubeLut::k_min_size || lut.size > asset::CubeLut::k_max_size ||
        lut.rgb.size() != static_cast<std::size_t>(lut.size) * lut.size * lut.size * 3) {
        return std::unexpected(core::Error{"color LUT: malformed table"});
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
