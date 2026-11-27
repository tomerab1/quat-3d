// Screenshot encoding — see include/engine/remote/image_encode.hpp.

#include "engine/remote/image_encode.hpp"

#include <algorithm>
#include <array>
#include <cmath>
#include <limits>

#include <stb_image_write.h>

namespace engine::remote {

namespace {

// 8-bit sRGB -> linear.
[[nodiscard]] const std::array<float, 256>& decode_lut() {
    static const std::array<float, 256> lut = [] {
        std::array<float, 256> t{};
        for (std::size_t i = 0; i < t.size(); ++i) {
            const float c = static_cast<float>(i) / 255.0F;
            t[i] = c <= 0.04045F ? c / 12.92F : std::pow((c + 0.055F) / 1.055F, 2.4F);
        }
        return t;
    }();
    return lut;
}

// Linear [0, 1] -> 8-bit sRGB, sampled finely enough (4096 steps) that the
// table never collapses distinct dark output codes.
constexpr std::size_t k_encode_steps = 4096;
[[nodiscard]] const std::array<std::uint8_t, k_encode_steps + 1>& encode_lut() {
    static const std::array<std::uint8_t, k_encode_steps + 1> lut = [] {
        std::array<std::uint8_t, k_encode_steps + 1> t{};
        for (std::size_t i = 0; i < t.size(); ++i) {
            const float l = static_cast<float>(i) / static_cast<float>(k_encode_steps);
            const float s = l <= 0.0031308F ? l * 12.92F : 1.055F * std::pow(l, 1.0F / 2.4F) - 0.055F;
            t[i] = static_cast<std::uint8_t>(std::lround(std::clamp(s, 0.0F, 1.0F) * 255.0F));
        }
        return t;
    }();
    return lut;
}

[[nodiscard]] std::uint8_t to_srgb8(float linear) {
    const float x = std::clamp(linear, 0.0F, 1.0F) * static_cast<float>(k_encode_steps);
    return encode_lut()[static_cast<std::size_t>(std::lround(x))];
}

// Box-filter footprint of each output sample along one axis: output pixel i
// covers source interval [i * ratio, (i + 1) * ratio), weighted by overlap.
struct Tap {
    std::uint32_t first = 0;
    std::vector<float> weights;
};

[[nodiscard]] std::vector<Tap> taps(std::uint32_t src, std::uint32_t dst) {
    std::vector<Tap> out(dst);
    const double ratio = static_cast<double>(src) / static_cast<double>(dst);
    for (std::uint32_t i = 0; i < dst; ++i) {
        const double lo = static_cast<double>(i) * ratio;
        const double hi = std::min(static_cast<double>(src), lo + ratio);
        const auto first = static_cast<std::uint32_t>(std::floor(lo));
        const auto last = std::min(src - 1, static_cast<std::uint32_t>(std::ceil(hi)) - 1);
        Tap& t = out[i];
        t.first = first;
        float sum = 0.0F;
        for (std::uint32_t s = first; s <= last; ++s) {
            const double cover =
                std::min(hi, static_cast<double>(s) + 1.0) - std::max(lo, static_cast<double>(s));
            const float w = static_cast<float>(std::max(0.0, cover));
            t.weights.push_back(w);
            sum += w;
        }
        for (float& w : t.weights) w /= sum > 0.0F ? sum : 1.0F;
    }
    return out;
}

void append_bytes(void* context, void* data, int size) {
    auto* out = static_cast<std::vector<std::uint8_t>*>(context);
    const auto* bytes = static_cast<const std::uint8_t*>(data);
    out->insert(out->end(), bytes, bytes + size);
}

} // namespace

std::expected<RgbImage, core::Error> downscale_srgb(std::span<const std::uint8_t> texels,
                                                    std::uint32_t width, std::uint32_t height,
                                                    std::size_t row_pitch, bool bgra,
                                                    std::uint32_t max_size) {
    if (width == 0 || height == 0 || max_size == 0) {
        return std::unexpected(core::Error{"screenshot: empty image"});
    }
    if (row_pitch < static_cast<std::size_t>(width) * 4 ||
        texels.size() < row_pitch * (height - 1) + static_cast<std::size_t>(width) * 4) {
        return std::unexpected(core::Error{"screenshot: pixel buffer too small"});
    }
    const int r_off = bgra ? 2 : 0;
    const int b_off = bgra ? 0 : 2;

    RgbImage out;
    const std::uint32_t longest = std::max(width, height);
    if (longest <= max_size) { // no resampling: swizzle only
        out.width = width;
        out.height = height;
        out.pixels.resize(static_cast<std::size_t>(width) * height * 3);
        for (std::uint32_t y = 0; y < height; ++y) {
            const std::uint8_t* src = texels.data() + row_pitch * y;
            std::uint8_t* dst = out.pixels.data() + static_cast<std::size_t>(y) * width * 3;
            for (std::uint32_t x = 0; x < width; ++x) {
                dst[x * 3 + 0] = src[x * 4 + r_off];
                dst[x * 3 + 1] = src[x * 4 + 1];
                dst[x * 3 + 2] = src[x * 4 + b_off];
            }
        }
        return out;
    }

    const double scale = static_cast<double>(max_size) / static_cast<double>(longest);
    out.width = std::max(1U, static_cast<std::uint32_t>(std::lround(width * scale)));
    out.height = std::max(1U, static_cast<std::uint32_t>(std::lround(height * scale)));
    const std::vector<Tap> tx = taps(width, out.width);
    const std::vector<Tap> ty = taps(height, out.height);
    const auto& lin = decode_lut();

    // Horizontal pass into linear floats, then vertical pass + encode.
    std::vector<float> mid(static_cast<std::size_t>(height) * out.width * 3);
    for (std::uint32_t y = 0; y < height; ++y) {
        const std::uint8_t* src = texels.data() + row_pitch * y;
        float* row = mid.data() + static_cast<std::size_t>(y) * out.width * 3;
        for (std::uint32_t x = 0; x < out.width; ++x) {
            float acc[3] = {0.0F, 0.0F, 0.0F};
            const Tap& t = tx[x];
            for (std::size_t k = 0; k < t.weights.size(); ++k) {
                const std::uint8_t* p = src + (t.first + k) * 4;
                acc[0] += t.weights[k] * lin[p[r_off]];
                acc[1] += t.weights[k] * lin[p[1]];
                acc[2] += t.weights[k] * lin[p[b_off]];
            }
            row[x * 3 + 0] = acc[0];
            row[x * 3 + 1] = acc[1];
            row[x * 3 + 2] = acc[2];
        }
    }
    out.pixels.resize(static_cast<std::size_t>(out.width) * out.height * 3);
    for (std::uint32_t y = 0; y < out.height; ++y) {
        const Tap& t = ty[y];
        std::uint8_t* dst = out.pixels.data() + static_cast<std::size_t>(y) * out.width * 3;
        for (std::uint32_t i = 0; i < out.width * 3; ++i) {
            float acc = 0.0F;
            for (std::size_t k = 0; k < t.weights.size(); ++k) {
                acc += t.weights[k] * mid[(static_cast<std::size_t>(t.first) + k) * out.width * 3 + i];
            }
            dst[i] = to_srgb8(acc);
        }
    }
    return out;
}

std::expected<std::vector<std::uint8_t>, core::Error> encode_png(const RgbImage& image) {
    if (image.width == 0 || image.height == 0 ||
        image.pixels.size() != static_cast<std::size_t>(image.width) * image.height * 3 ||
        image.width > static_cast<std::uint32_t>(std::numeric_limits<int>::max() / 3) ||
        image.height > static_cast<std::uint32_t>(std::numeric_limits<int>::max())) {
        return std::unexpected(core::Error{"png: invalid image"});
    }
    std::vector<std::uint8_t> png;
    const int ok = stbi_write_png_to_func(append_bytes, &png, static_cast<int>(image.width),
                                          static_cast<int>(image.height), 3, image.pixels.data(),
                                          static_cast<int>(image.width * 3));
    if (ok == 0 || png.empty()) return std::unexpected(core::Error{"png: encoding failed"});
    return png;
}

std::expected<EncodedImage, core::Error> encode_screenshot(std::span<const std::uint8_t> texels,
                                                           std::uint32_t width,
                                                           std::uint32_t height,
                                                           std::size_t row_pitch, bool bgra,
                                                           std::uint32_t max_size) {
    auto rgb = downscale_srgb(texels, width, height, row_pitch, bgra, max_size);
    if (!rgb) return std::unexpected(rgb.error());
    auto png = encode_png(*rgb);
    if (!png) return std::unexpected(png.error());
    return EncodedImage{rgb->width, rgb->height, std::move(*png)};
}

std::string base64_encode(std::span<const std::uint8_t> bytes) {
    static constexpr char alphabet[] =
        "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
    std::string out;
    out.reserve((bytes.size() + 2) / 3 * 4);
    std::size_t i = 0;
    for (; i + 3 <= bytes.size(); i += 3) {
        const std::uint32_t v = (static_cast<std::uint32_t>(bytes[i]) << 16) |
                                (static_cast<std::uint32_t>(bytes[i + 1]) << 8) | bytes[i + 2];
        out.push_back(alphabet[(v >> 18) & 63]);
        out.push_back(alphabet[(v >> 12) & 63]);
        out.push_back(alphabet[(v >> 6) & 63]);
        out.push_back(alphabet[v & 63]);
    }
    if (const std::size_t rest = bytes.size() - i; rest > 0) {
        std::uint32_t v = static_cast<std::uint32_t>(bytes[i]) << 16;
        if (rest == 2) v |= static_cast<std::uint32_t>(bytes[i + 1]) << 8;
        out.push_back(alphabet[(v >> 18) & 63]);
        out.push_back(alphabet[(v >> 12) & 63]);
        out.push_back(rest == 2 ? alphabet[(v >> 6) & 63] : '=');
        out.push_back('=');
    }
    return out;
}

} // namespace engine::remote
