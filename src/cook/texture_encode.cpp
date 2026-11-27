// RGBA8 image -> (resized to a multiple of 4) mip chain -> KTX2 with Basis
// Universal (ETC1S or UASTC + Zstd) via libktx.

#include <ktx.h>

#include <algorithm>
#include <cmath>
#include <cstdlib>
#include <cstring>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <thread>
#include <vector>

#include "cook/internal.hpp"
#include "engine/asset/alpha_coverage.hpp"

namespace engine::cook::detail {

namespace {

// Linear-light float RGBA image.
struct Image {
    std::uint32_t      width = 0;
    std::uint32_t      height = 0;
    std::vector<float> px; // width * height * 4

    [[nodiscard]] const float* at(std::uint32_t x, std::uint32_t y) const {
        return px.data() + (static_cast<std::size_t>(y) * width + x) * 4;
    }
};

[[nodiscard]] float srgb_to_linear(float c) {
    return c <= 0.04045F ? c / 12.92F : std::pow((c + 0.055F) / 1.055F, 2.4F);
}

[[nodiscard]] float linear_to_srgb(float c) {
    return c <= 0.0031308F ? c * 12.92F : 1.055F * std::pow(c, 1.0F / 2.4F) - 0.055F;
}

[[nodiscard]] Image to_linear(std::span<const std::byte> rgba, std::uint32_t w, std::uint32_t h,
                              bool srgb) {
    Image img{w, h, std::vector<float>(static_cast<std::size_t>(w) * h * 4)};
    for (std::size_t i = 0; i < img.px.size(); ++i) {
        const float v = static_cast<float>(std::to_integer<std::uint8_t>(rgba[i])) / 255.0F;
        img.px[i] = (srgb && i % 4 != 3) ? srgb_to_linear(v) : v;
    }
    return img;
}

[[nodiscard]] std::uint8_t to_unorm8(float v) {
    return static_cast<std::uint8_t>(std::lround(std::clamp(v, 0.0F, 1.0F) * 255.0F));
}

// Bilinear resample (texel centres aligned), used to reach multiple-of-4
// dimensions as KHR_texture_basisu requires.
[[nodiscard]] Image resample(const Image& src, std::uint32_t w, std::uint32_t h) {
    Image dst{w, h, std::vector<float>(static_cast<std::size_t>(w) * h * 4)};
    const float sx = static_cast<float>(src.width) / static_cast<float>(w);
    const float sy = static_cast<float>(src.height) / static_cast<float>(h);
    for (std::uint32_t y = 0; y < h; ++y) {
        const float fy = std::max((static_cast<float>(y) + 0.5F) * sy - 0.5F, 0.0F);
        const auto y0 = std::min(static_cast<std::uint32_t>(fy), src.height - 1);
        const std::uint32_t y1 = std::min(y0 + 1, src.height - 1);
        const float ty = fy - static_cast<float>(y0);
        for (std::uint32_t x = 0; x < w; ++x) {
            const float fx = std::max((static_cast<float>(x) + 0.5F) * sx - 0.5F, 0.0F);
            const auto x0 = std::min(static_cast<std::uint32_t>(fx), src.width - 1);
            const std::uint32_t x1 = std::min(x0 + 1, src.width - 1);
            const float tx = fx - static_cast<float>(x0);
            float* d = dst.px.data() + (static_cast<std::size_t>(y) * w + x) * 4;
            for (int c = 0; c < 4; ++c) {
                const float top = src.at(x0, y0)[c] * (1 - tx) + src.at(x1, y0)[c] * tx;
                const float bot = src.at(x0, y1)[c] * (1 - tx) + src.at(x1, y1)[c] * tx;
                d[c] = top * (1 - ty) + bot * ty;
            }
        }
    }
    return dst;
}

// Renormalises a normal map texel (stored as n * 0.5 + 0.5).
void renormalize(float* p) {
    float n[3] = {p[0] * 2 - 1, p[1] * 2 - 1, p[2] * 2 - 1};
    const float len = std::sqrt(n[0] * n[0] + n[1] * n[1] + n[2] * n[2]);
    if (len < 1e-6F) {
        n[0] = 0;
        n[1] = 0;
        n[2] = 1;
    } else {
        for (float& c : n) c /= len;
    }
    for (int c = 0; c < 3; ++c) p[c] = n[c] * 0.5F + 0.5F;
}

// 2x2 box downsample in linear light; odd edges fold the last texel in.
[[nodiscard]] Image downsample(const Image& src, bool normal) {
    const std::uint32_t w = std::max(src.width / 2, 1U);
    const std::uint32_t h = std::max(src.height / 2, 1U);
    Image dst{w, h, std::vector<float>(static_cast<std::size_t>(w) * h * 4)};
    for (std::uint32_t y = 0; y < h; ++y) {
        for (std::uint32_t x = 0; x < w; ++x) {
            const std::uint32_t xs[2] = {std::min(2 * x, src.width - 1),
                                         std::min(2 * x + 1, src.width - 1)};
            const std::uint32_t ys[2] = {std::min(2 * y, src.height - 1),
                                         std::min(2 * y + 1, src.height - 1)};
            float* d = dst.px.data() + (static_cast<std::size_t>(y) * w + x) * 4;
            for (int c = 0; c < 4; ++c) {
                d[c] = 0.25F * (src.at(xs[0], ys[0])[c] + src.at(xs[1], ys[0])[c] +
                                src.at(xs[0], ys[1])[c] + src.at(xs[1], ys[1])[c]);
            }
            if (normal) renormalize(d);
        }
    }
    return dst;
}

// Packs `components` channels (R, RGB or RGBA) as 8-bit.
[[nodiscard]] std::vector<std::uint8_t> pack(const Image& img, std::uint32_t components,
                                             bool srgb) {
    std::vector<std::uint8_t> out(static_cast<std::size_t>(img.width) * img.height * components);
    for (std::size_t i = 0; i < static_cast<std::size_t>(img.width) * img.height; ++i) {
        for (std::uint32_t c = 0; c < components; ++c) {
            const float v = img.px[i * 4 + c];
            out[i * components + c] = to_unorm8(srgb && c < 3 ? linear_to_srgb(v) : v);
        }
    }
    return out;
}

[[nodiscard]] std::uint32_t vk_format_for(std::uint32_t components, bool srgb) {
    switch (components) {
        case 1:  return srgb ? 15U : 9U;   // VK_FORMAT_R8_{SRGB,UNORM}
        case 3:  return srgb ? 29U : 23U;  // VK_FORMAT_R8G8B8_{SRGB,UNORM}
        default: return srgb ? 43U : 37U;  // VK_FORMAT_R8G8B8A8_{SRGB,UNORM}
    }
}

struct KtxDeleter {
    void operator()(ktxTexture2* t) const noexcept { ktxTexture2_Destroy(t); }
};

[[nodiscard]] std::string ktx_error(const char* what, ktx_error_code_e e) {
    return std::string(what) + ": " + ktxErrorString(e);
}

} // namespace

std::expected<std::vector<std::byte>, core::Error>
encode_image_ktx2(std::span<const std::byte> rgba, std::uint32_t width, std::uint32_t height,
                  asset::TextureRole role, asset::TextureColorSpace color_space,
                  const TextureCookOptions& options, std::optional<float> alpha_test_cutoff) {
    if (width == 0 || height == 0 ||
        rgba.size() != static_cast<std::size_t>(width) * height * 4) {
        return fail("image data does not match its extent");
    }
    const bool normal = role == asset::TextureRole::normal;
    const bool srgb = color_space == asset::TextureColorSpace::srgb && !normal;
    Image base = to_linear(rgba, width, height, srgb);
    const std::uint32_t w4 = (width + 3) / 4 * 4;
    const std::uint32_t h4 = (height + 3) / 4 * 4;
    if (w4 != width || h4 != height) base = resample(base, w4, h4);

    std::uint32_t components = 4;
    if (role == asset::TextureRole::single_channel) {
        components = 1;
    } else if (normal) {
        components = 3;
    } else {
        bool opaque = true;
        for (std::size_t i = 3; i < base.px.size() && opaque; i += 4) opaque = base.px[i] >= 1.0F;
        if (opaque) components = 3;
    }

    std::vector<Image> chain;
    if (options.mipmaps && components == 4 && alpha_test_cutoff.has_value()) {
        // glTF MASK base colour: alpha-test coverage kept at every level.
        const std::vector<std::uint8_t> rgba8 = pack(base, 4, srgb);
        for (const asset::MipLevelRgba8& level : asset::build_alpha_tested_mips(
                 rgba8, base.width, base.height, srgb, *alpha_test_cutoff)) {
            chain.push_back(to_linear(std::as_bytes(std::span(level.pixels)), level.width,
                                      level.height, srgb));
        }
    }
    if (chain.empty()) {
        chain.push_back(std::move(base));
        while (options.mipmaps && (chain.back().width > 1 || chain.back().height > 1)) {
            chain.push_back(downsample(chain.back(), normal));
        }
    }

    ktxTextureCreateInfo ci{};
    ci.vkFormat = vk_format_for(components, srgb);
    ci.baseWidth = w4;
    ci.baseHeight = h4;
    ci.baseDepth = 1;
    ci.numDimensions = 2;
    ci.numLevels = static_cast<ktx_uint32_t>(chain.size());
    ci.numLayers = 1;
    ci.numFaces = 1;
    ci.isArray = KTX_FALSE;
    ci.generateMipmaps = KTX_FALSE;
    ktxTexture2* raw_tex = nullptr;
    if (const auto r = ktxTexture2_Create(&ci, KTX_TEXTURE_CREATE_ALLOC_STORAGE, &raw_tex);
        r != KTX_SUCCESS) {
        return fail(ktx_error("ktxTexture2_Create", r));
    }
    std::unique_ptr<ktxTexture2, KtxDeleter> tex(raw_tex);
    for (std::size_t level = 0; level < chain.size(); ++level) {
        const std::vector<std::uint8_t> bytes = pack(chain[level], components, srgb);
        if (const auto r = ktxTexture_SetImageFromMemory(ktxTexture(tex.get()),
                                                         static_cast<ktx_uint32_t>(level), 0, 0,
                                                         bytes.data(), bytes.size());
            r != KTX_SUCCESS) {
            return fail(ktx_error("ktxTexture_SetImageFromMemory", r));
        }
    }

    const BasisCodec codec = normal ? options.normal_codec : options.color_codec;
    ktxBasisParams params{};
    params.structSize = sizeof(params);
    params.uastc = codec == BasisCodec::uastc ? KTX_TRUE : KTX_FALSE;
    params.threadCount =
        options.threads != 0 ? options.threads : std::max(std::thread::hardware_concurrency(), 1U);
    params.compressionLevel = KTX_ETC1S_DEFAULT_COMPRESSION_LEVEL;
    params.qualityLevel = std::clamp(options.etc1s_quality, 1U, 255U);
    params.uastcFlags = std::min(options.uastc_level, 4U);
    params.uastcRDO = options.uastc_rdo > 0.0F ? KTX_TRUE : KTX_FALSE;
    params.uastcRDOQualityScalar = options.uastc_rdo;
    params.normalMap = (normal && options.normal_xy_layout) ? KTX_TRUE : KTX_FALSE;
    if (const auto r = ktxTexture2_CompressBasisEx(tex.get(), &params); r != KTX_SUCCESS) {
        return fail(ktx_error("Basis encode", r));
    }
    if (codec == BasisCodec::uastc && options.zstd_level > 0) {
        if (const auto r = ktxTexture2_DeflateZstd(tex.get(), std::min(options.zstd_level, 22U));
            r != KTX_SUCCESS) {
            return fail(ktx_error("Zstd supercompression", r));
        }
    }

    ktx_uint8_t* out = nullptr;
    ktx_size_t out_size = 0;
    if (const auto r = ktxTexture2_WriteToMemory(tex.get(), &out, &out_size); r != KTX_SUCCESS) {
        return fail(ktx_error("ktxTexture2_WriteToMemory", r));
    }
    std::vector<std::byte> bytes(out_size);
    std::memcpy(bytes.data(), out, out_size);
    std::free(out); // NOLINT(cppcoreguidelines-no-malloc): libktx allocates with malloc
    return bytes;
}

} // namespace engine::cook::detail

namespace engine::cook {

std::expected<std::vector<std::byte>, core::Error>
encode_ktx2(std::span<const std::byte> rgba, std::uint32_t width, std::uint32_t height,
            asset::TextureRole role, asset::TextureColorSpace color_space,
            const TextureCookOptions& options) {
    return detail::encode_image_ktx2(rgba, width, height, role, color_space, options);
}

} // namespace engine::cook
