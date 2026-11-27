// Coverage-preserving alpha mip chains (Castaño 2010). See alpha_coverage.hpp.

#include "engine/asset/alpha_coverage.hpp"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <span>
#include <vector>

namespace engine::asset {

namespace {

// Sub-samples per texel along each axis for the coverage estimate (NVTT).
constexpr std::uint32_t k_sub = 4;
// Histogram resolution of the coverage quantile.
constexpr std::uint32_t k_bins = 4096;

// A level kept in float while the chain is built: rgb is the alpha-weighted
// mean colour (linear light for sRGB data), a the box-filtered, unscaled alpha.
struct FloatLevel {
    std::uint32_t      width = 0;
    std::uint32_t      height = 0;
    std::vector<float> rgba;

    [[nodiscard]] float alpha(std::uint32_t x, std::uint32_t y) const {
        return rgba[(static_cast<std::size_t>(y) * width + x) * 4 + 3];
    }
};

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

[[nodiscard]] FloatLevel to_float(std::span<const std::uint8_t> rgba, std::uint32_t width,
                                  std::uint32_t height, bool srgb) {
    FloatLevel out{width, height, std::vector<float>(rgba.size())};
    std::array<float, 256> decode{};
    for (std::size_t i = 0; i < decode.size(); ++i) {
        const float v = static_cast<float>(i) / 255.0F;
        decode[i] = srgb ? srgb_to_linear(v) : v;
    }
    for (std::size_t i = 0; i < rgba.size(); i += 4) {
        out.rgba[i] = decode[rgba[i]];
        out.rgba[i + 1] = decode[rgba[i + 1]];
        out.rgba[i + 2] = decode[rgba[i + 2]];
        out.rgba[i + 3] = static_cast<float>(rgba[i + 3]) / 255.0F;
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

[[nodiscard]] FloatLevel downsample(const FloatLevel& src) {
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
                    const float* p =
                        &src.rgba[(static_cast<std::size_t>(vy.index) * src.width + vx.index) * 4];
                    const float wa = w * p[3];
                    for (int c = 0; c < 3; ++c) {
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
            for (int c = 0; c < 3; ++c) {
                o[c] = colour_w > 1e-6F ? colour[c] / colour_w : plain[c];
            }
            o[3] = alpha;
        }
    }
    return dst;
}

// Calls `fn(alpha)` for the 4x4 bilinear sub-samples of every texel (NVTT's
// coverage estimate; edges clamp), each texel's alpha mapped by `texel` first.
template <typename Texel, typename Fn>
void for_each_subsample(const FloatLevel& level, Texel&& texel, Fn&& fn) {
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
[[nodiscard]] float coverage_of(const FloatLevel& level, float cutoff, float scale,
                                bool quantize) {
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
    std::vector<std::size_t> hist(k_bins, 0);
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

[[nodiscard]] MipLevelRgba8 to_unorm(const FloatLevel& level, bool srgb, float alpha_scale) {
    MipLevelRgba8 out{level.width, level.height,
                      std::vector<std::uint8_t>(level.rgba.size())};
    for (std::size_t i = 0; i < level.rgba.size(); i += 4) {
        for (std::size_t c = 0; c < 3; ++c) {
            const float v = level.rgba[i + c];
            out.pixels[i + c] = to_unorm8(srgb ? linear_to_srgb(std::clamp(v, 0.0F, 1.0F)) : v);
        }
        out.pixels[i + 3] = to_unorm8(level.rgba[i + 3] * alpha_scale);
    }
    return out;
}

[[nodiscard]] std::vector<MipLevelRgba8> build_chain(std::span<const std::uint8_t> rgba,
                                                     std::uint32_t width, std::uint32_t height,
                                                     bool srgb, const float* cutoff) {
    std::vector<MipLevelRgba8> out;
    if (!valid_input(rgba, width, height)) return out;
    out.push_back({width, height, std::vector<std::uint8_t>(rgba.begin(), rgba.end())});
    FloatLevel level = to_float(rgba, width, height, srgb);
    const float target = cutoff != nullptr ? coverage_of(level, *cutoff, 1.0F, true) : 0.0F;
    while (level.width > 1 || level.height > 1) {
        level = downsample(level);
        const float scale = cutoff != nullptr ? coverage_scale(level, *cutoff, target) : 1.0F;
        out.push_back(to_unorm(level, srgb, scale));
    }
    return out;
}

} // namespace

float alpha_test_coverage(std::span<const std::uint8_t> rgba, std::uint32_t width,
                          std::uint32_t height, float cutoff, float alpha_scale) {
    if (!valid_input(rgba, width, height)) return 0.0F;
    return coverage_of(to_float(rgba, width, height, false), cutoff, alpha_scale, false);
}

std::vector<MipLevelRgba8> build_alpha_tested_mips(std::span<const std::uint8_t> rgba,
                                                   std::uint32_t width, std::uint32_t height,
                                                   bool srgb, float cutoff) {
    return build_chain(rgba, width, height, srgb, &cutoff);
}

std::vector<MipLevelRgba8> build_box_filtered_mips(std::span<const std::uint8_t> rgba,
                                                   std::uint32_t width, std::uint32_t height,
                                                   bool srgb) {
    return build_chain(rgba, width, height, srgb, nullptr);
}

} // namespace engine::asset
