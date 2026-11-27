// Coverage-preserving alpha mip chains (Castaño 2010). See alpha_coverage.hpp.
//
// Memory: level 0 is read in place as RGBA8 (never copied to float); each
// coarser level is built in float from the previous one, and only those two
// float levels are alive at a time. Every level is written straight into its
// destination (the caller's PixelBuffer at load time), so the runtime path's
// whole footprint is known before anything is allocated (alpha_tested_mips_cost)
// and is claimed from the load's DecodeBudget first.

#include "engine/asset/alpha_coverage.hpp"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <span>
#include <utility>
#include <vector>

namespace engine::asset {

namespace {

// Sub-samples per texel along each axis for the coverage estimate (NVTT).
constexpr std::uint32_t k_sub = 4;
// Histogram resolution of the coverage quantile.
constexpr std::uint32_t k_bins = 4096;
// Bytes per texel of a float working level.
constexpr std::uint64_t k_float_texel = 4 * sizeof(float);

[[nodiscard]] float srgb_to_linear(float c) {
    return c <= 0.04045F ? c / 12.92F : std::pow((c + 0.055F) / 1.055F, 2.4F);
}

[[nodiscard]] float linear_to_srgb(float c) {
    return c <= 0.0031308F ? c * 12.92F : 1.055F * std::pow(c, 1.0F / 2.4F) - 0.055F;
}

[[nodiscard]] std::uint8_t to_unorm8(float v) {
    return static_cast<std::uint8_t>(std::lround(std::clamp(v, 0.0F, 1.0F) * 255.0F));
}

[[nodiscard]] bool valid_input(std::span<const std::uint8_t> rgba, std::uint32_t width,
                               std::uint32_t height) {
    return width > 0 && height > 0 &&
           rgba.size() == static_cast<std::size_t>(width) * height * 4;
}

[[nodiscard]] std::uint32_t mip_extent(std::uint32_t base, std::uint32_t level) {
    return std::max(base >> level, 1U);
}

// Level 0 read in place: RGBA8, colour decoded to linear through a table.
struct U8Level {
    const std::uint8_t*          px = nullptr;
    std::uint32_t                width = 0;
    std::uint32_t                height = 0;
    const std::array<float, 256>* decode = nullptr;

    [[nodiscard]] float alpha(std::uint32_t x, std::uint32_t y) const {
        return static_cast<float>(px[(static_cast<std::size_t>(y) * width + x) * 4 + 3]) / 255.0F;
    }
    void texel(std::uint32_t x, std::uint32_t y, float out[4]) const {
        const std::uint8_t* p = px + (static_cast<std::size_t>(y) * width + x) * 4;
        out[0] = (*decode)[p[0]];
        out[1] = (*decode)[p[1]];
        out[2] = (*decode)[p[2]];
        out[3] = static_cast<float>(p[3]) / 255.0F;
    }
};

// A coarser level kept in float while the chain is built: rgb is the
// alpha-weighted mean colour (linear light for sRGB data), a the
// box-filtered, unscaled alpha.
struct FloatLevel {
    std::uint32_t      width = 0;
    std::uint32_t      height = 0;
    std::vector<float> rgba;

    [[nodiscard]] float alpha(std::uint32_t x, std::uint32_t y) const {
        return rgba[(static_cast<std::size_t>(y) * width + x) * 4 + 3];
    }
    void texel(std::uint32_t x, std::uint32_t y, float out[4]) const {
        std::memcpy(out, &rgba[(static_cast<std::size_t>(y) * width + x) * 4], 4 * sizeof(float));
    }
};

[[nodiscard]] std::array<float, 256> decode_table(bool srgb) {
    std::array<float, 256> out{};
    for (std::size_t i = 0; i < out.size(); ++i) {
        const float v = static_cast<float>(i) / 255.0F;
        out[i] = srgb ? srgb_to_linear(v) : v;
    }
    return out;
}

// Source texels (index, area weight) covered by destination texel `d` when
// `src` texels shrink to `dst` (dst <= src): an area-weighted box filter, so
// odd sizes spread the middle texel over both neighbours.
struct Tap {
    std::uint32_t index = 0;
    float         weight = 0.0F;
};
[[nodiscard]] std::vector<std::vector<Tap>> box_taps(std::uint32_t src, std::uint32_t dst) {
    std::vector<std::vector<Tap>> out(dst);
    const double ratio = static_cast<double>(src) / static_cast<double>(dst);
    for (std::uint32_t d = 0; d < dst; ++d) {
        const double lo = d * ratio;
        const double hi = (d + 1) * ratio;
        auto first = static_cast<std::uint32_t>(std::floor(lo));
        for (std::uint32_t s = first; s < src && static_cast<double>(s) < hi; ++s) {
            const double overlap = std::min<double>(hi, s + 1.0) - std::max<double>(lo, s);
            if (overlap > 1e-9) out[d].push_back({s, static_cast<float>(overlap / ratio)});
        }
    }
    return out;
}

template <typename Src>
[[nodiscard]] FloatLevel downsample(const Src& src) {
    FloatLevel dst;
    dst.width = std::max(src.width / 2, 1U);
    dst.height = std::max(src.height / 2, 1U);
    dst.rgba.assign(static_cast<std::size_t>(dst.width) * dst.height * 4, 0.0F);
    const auto tx = box_taps(src.width, dst.width);
    const auto ty = box_taps(src.height, dst.height);
    for (std::uint32_t y = 0; y < dst.height; ++y) {
        for (std::uint32_t x = 0; x < dst.width; ++x) {
            float colour_w = 0.0F;
            std::array<float, 3> colour{};
            std::array<float, 3> plain{};
            float alpha = 0.0F;
            for (const Tap& vy : ty[y]) {
                for (const Tap& vx : tx[x]) {
                    const float w = vx.weight * vy.weight;
                    float p[4];
                    src.texel(vx.index, vy.index, p);
                    const float wa = w * p[3];
                    for (std::size_t c = 0; c < 3; ++c) {
                        colour[c] += wa * p[c];
                        plain[c] += w * p[c];
                    }
                    colour_w += wa;
                    alpha += w * p[3];
                }
            }
            float* o = &dst.rgba[(static_cast<std::size_t>(y) * dst.width + x) * 4];
            // Fully transparent footprint: keep the plain mean (never shown,
            // but keeps the colour continuous for bilinear neighbours).
            for (std::size_t c = 0; c < 3; ++c) {
                o[c] = colour_w > 1e-6F ? colour[c] / colour_w : plain[c];
            }
            o[3] = alpha;
        }
    }
    return dst;
}

// Calls `fn(alpha)` for the 4x4 bilinear sub-samples of every texel (NVTT's
// coverage estimate; edges clamp), each texel's alpha mapped by `texel` first.
template <typename Level, typename Texel, typename Fn>
void for_each_subsample(const Level& level, Texel&& texel, Fn&& fn) {
    for (std::uint32_t y = 0; y < level.height; ++y) {
        const std::uint32_t y1 = std::min(y + 1, level.height - 1);
        for (std::uint32_t x = 0; x < level.width; ++x) {
            const std::uint32_t x1 = std::min(x + 1, level.width - 1);
            const float a00 = texel(level.alpha(x, y));
            const float a10 = texel(level.alpha(x1, y));
            const float a01 = texel(level.alpha(x, y1));
            const float a11 = texel(level.alpha(x1, y1));
            for (std::uint32_t sy = 0; sy < k_sub; ++sy) {
                const float fy = (static_cast<float>(sy) + 0.5F) / static_cast<float>(k_sub);
                const float a0 = a00 + (a01 - a00) * fy;
                const float a1 = a10 + (a11 - a10) * fy;
                for (std::uint32_t sx = 0; sx < k_sub; ++sx) {
                    const float fx = (static_cast<float>(sx) + 0.5F) / static_cast<float>(k_sub);
                    fn(a0 + (a1 - a0) * fx);
                }
            }
        }
    }
}

// Alpha-test coverage of `level` with its alpha scaled by `scale` and, when
// `quantize`, rounded to 8 bits per texel as it is stored.
template <typename Level>
[[nodiscard]] float coverage_of(const Level& level, float cutoff, float scale, bool quantize) {
    std::size_t passed = 0;
    std::size_t total = 0;
    const auto texel = [&](float a) {
        const float v = std::clamp(a * scale, 0.0F, 1.0F);
        return quantize ? static_cast<float>(to_unorm8(v)) / 255.0F : v;
    };
    for_each_subsample(level, texel, [&](float a) {
        passed += a >= cutoff ? 1U : 0U;
        ++total;
    });
    return total == 0 ? 0.0F : static_cast<float>(passed) / static_cast<float>(total);
}

// The alpha scale that gives `level` the alpha-test coverage `target`. The
// threshold t with fraction(alpha >= t) = target is a quantile of the
// sub-sample alphas (read from a histogram), and scale = cutoff / t. The
// stored alpha is 8-bit, which moves sub-samples within half a step of the
// threshold across it, so the final pick evaluates the stored coverage at
// the thresholds a few 8-bit steps around t (and the unscaled cutoff) and
// keeps the closest.
[[nodiscard]] float coverage_scale(const FloatLevel& level, float cutoff, float target) {
    if (target <= 0.0F) return 1.0F; // nothing passed at level 0: nothing to keep
    const float scale_bins = static_cast<float>(k_bins - 1);
    std::array<std::size_t, k_bins> hist{};
    std::size_t total = 0;
    const auto identity = [](float a) { return a; };
    for_each_subsample(level, identity, [&](float a) {
        ++hist[static_cast<std::size_t>(std::clamp(a, 0.0F, 1.0F) * scale_bins + 0.5F)];
        ++total;
    });
    const double wanted = static_cast<double>(target) * static_cast<double>(total);
    std::size_t above = 0;
    std::size_t bin = k_bins - 1;
    for (;; --bin) {
        above += hist[bin];
        if (static_cast<double>(above) >= wanted || bin == 0) break;
    }
    const float t0 = static_cast<float>(bin) / scale_bins;

    float best_scale = 1.0F;
    float best_error = std::abs(coverage_of(level, cutoff, 1.0F, true) - target);
    constexpr int k_steps = 3;
    for (int j = -k_steps; j <= k_steps; ++j) {
        // Never a zero threshold: it would pass fully transparent texels.
        const float t = std::max(t0 + static_cast<float>(j) / 255.0F, 0.5F / 255.0F);
        if (t > 1.0F) continue;
        const float scale = cutoff / t;
        const float error = std::abs(coverage_of(level, cutoff, scale, true) - target);
        if (error < best_error) {
            best_error = error;
            best_scale = scale;
        }
    }
    return best_scale;
}

void write_level(const FloatLevel& level, bool srgb, float alpha_scale, std::uint8_t* out) {
    for (std::size_t i = 0; i < level.rgba.size(); i += 4) {
        for (std::size_t c = 0; c < 3; ++c) {
            const float v = level.rgba[i + c];
            out[i + c] = to_unorm8(srgb ? linear_to_srgb(std::clamp(v, 0.0F, 1.0F)) : v);
        }
        out[i + 3] = to_unorm8(level.rgba[i + 3] * alpha_scale);
    }
}

// Builds the chain of `rgba` (validated by the caller), asking `dest(level,
// width, height)` for the RGBA8 destination of each level. Level 0 is copied
// unless its destination is `rgba` itself; the coverage step runs when
// `cutoff` is given.
template <typename Dest>
void build_chain(std::span<const std::uint8_t> rgba, std::uint32_t width, std::uint32_t height,
                 bool srgb, const float* cutoff, Dest&& dest) {
    const std::array<float, 256> decode = decode_table(srgb);
    const U8Level base{rgba.data(), width, height, &decode};
    if (std::uint8_t* l0 = dest(0U, width, height); l0 != rgba.data()) {
        std::memcpy(l0, rgba.data(), rgba.size());
    }
    if (width == 1 && height == 1) return;
    const float target = cutoff != nullptr ? coverage_of(base, *cutoff, 1.0F, true) : 0.0F;
    FloatLevel level = downsample(base);
    for (std::uint32_t i = 1;; ++i) {
        const float scale = cutoff != nullptr ? coverage_scale(level, *cutoff, target) : 1.0F;
        write_level(level, srgb, scale, dest(i, level.width, level.height));
        if (level.width == 1 && level.height == 1) break;
        level = downsample(level); // the previous and the next level are alive here
    }
}

[[nodiscard]] std::vector<MipLevelRgba8> build_vectors(std::span<const std::uint8_t> rgba,
                                                       std::uint32_t width, std::uint32_t height,
                                                       bool srgb, const float* cutoff) {
    std::vector<MipLevelRgba8> out;
    if (!valid_input(rgba, width, height)) return out;
    build_chain(rgba, width, height, srgb, cutoff,
                [&](std::uint32_t, std::uint32_t w, std::uint32_t h) {
                    out.push_back({w, h, std::vector<std::uint8_t>(std::size_t{w} * h * 4)});
                    return out.back().pixels.data();
                });
    return out;
}

} // namespace

AlphaMipsCost alpha_tested_mips_cost(std::uint32_t width, std::uint32_t height) {
    AlphaMipsCost cost;
    if (width == 0 || height == 0) return cost;
    std::uint64_t previous_float = 0;
    for (std::uint32_t i = 0;; ++i) {
        const std::uint64_t texels =
            std::uint64_t{mip_extent(width, i)} * std::uint64_t{mip_extent(height, i)};
        cost.chain_bytes += texels * 4;
        if (i > 0) {
            // Level i is built while level i - 1 is still alive.
            cost.working_bytes =
                std::max(cost.working_bytes, previous_float + texels * k_float_texel);
            previous_float = texels * k_float_texel;
        }
        if (mip_extent(width, i) == 1 && mip_extent(height, i) == 1) break;
    }
    return cost;
}

float alpha_test_coverage(std::span<const std::uint8_t> rgba, std::uint32_t width,
                          std::uint32_t height, float cutoff, float alpha_scale) {
    if (!valid_input(rgba, width, height)) return 0.0F;
    const std::array<float, 256> decode = decode_table(false);
    return coverage_of(U8Level{rgba.data(), width, height, &decode}, cutoff, alpha_scale, false);
}

std::vector<MipLevelRgba8> build_alpha_tested_mips(std::span<const std::uint8_t> rgba,
                                                   std::uint32_t width, std::uint32_t height,
                                                   bool srgb, float cutoff) {
    return build_vectors(rgba, width, height, srgb, &cutoff);
}

std::vector<MipLevelRgba8> build_box_filtered_mips(std::span<const std::uint8_t> rgba,
                                                   std::uint32_t width, std::uint32_t height,
                                                   bool srgb) {
    return build_vectors(rgba, width, height, srgb, nullptr);
}

bool apply_alpha_tested_mips(TextureData& data, float cutoff, DecodeBudget* budget,
                             std::uint64_t max_image_bytes) {
    if (data.format != TexelFormat::rgba8 || !data.levels.empty() || data.pixels.empty()) {
        return false;
    }
    const std::span<const std::uint8_t> rgba(
        reinterpret_cast<const std::uint8_t*>(data.pixels.data()), data.pixels.size());
    if (!valid_input(rgba, data.width, data.height)) return false;

    // Everything is sized and claimed before the first allocation: the new
    // chain (level 0 included: the old image is released only at the end)
    // and the float working set, which is returned once the chain is built.
    const AlphaMipsCost cost = alpha_tested_mips_cost(data.width, data.height);
    if (cost.chain_bytes > max_image_bytes) return false;
    if (budget != nullptr && !budget->reserve(cost.chain_bytes + cost.working_bytes)) {
        return false;
    }
    PixelBuffer pixels = PixelBuffer::allocate(static_cast<std::size_t>(cost.chain_bytes));
    if (pixels.empty()) {
        if (budget != nullptr) budget->release(cost.chain_bytes + cost.working_bytes);
        return false;
    }
    std::vector<TextureLevel> levels;
    std::uint64_t offset = 0;
    build_chain(rgba, data.width, data.height, data.color_space == TextureColorSpace::srgb,
                &cutoff, [&](std::uint32_t, std::uint32_t w, std::uint32_t h) {
                    const std::uint64_t size = std::uint64_t{w} * h * 4;
                    levels.push_back({offset, size, w, h});
                    std::uint8_t* at = reinterpret_cast<std::uint8_t*>(pixels.data()) + offset;
                    offset += size;
                    return at;
                });
    // The working set is gone, and so is the single-level image the decode
    // claimed: the load is now charged for the chain alone.
    if (budget != nullptr) budget->release(cost.working_bytes + data.pixels.size());
    data.pixels = std::move(pixels);
    data.levels = std::move(levels);
    return true;
}

} // namespace engine::asset
