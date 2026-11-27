#pragma once

// Coverage-preserving mip chains for alpha-tested textures (glTF alphaMode
// MASK; ROADMAP R12.11, issue #35).
//
// A plain box-filtered mip chain averages thin opaque features (leaves,
// grass blades, fences) with the transparent texels around them, so their
// alpha falls below the cutoff a few mips down and foliage "thins out" with
// distance. Castaño (2010, "Computing Alpha Mipmaps"; NVIDIA Texture Tools)
// fixes it at build time: each level's alpha is rescaled so the fraction of
// the texture that passes the alpha test equals level 0's. Coverage is
// estimated with 4x4 bilinear sub-samples per texel, as NVTT does; the scale
// comes from a histogram quantile of those sub-samples, refined against the
// coverage of the 8-bit alpha actually stored.
//
// Colour is filtered with alpha weights (transparent texels carry no colour,
// so cut-out edges don't pick up the black or garbage colour around them) and
// in linear light for sRGB data. Everything here is CPU-only and pure, so it
// has a CPU reference test.

#include <cstdint>
#include <span>
#include <vector>

namespace engine::asset {

// One level of an RGBA8 mip chain, tightly packed rows.
struct MipLevelRgba8 {
    std::uint32_t             width = 0;
    std::uint32_t             height = 0;
    std::vector<std::uint8_t> pixels; // width * height * 4 bytes
};

// The fraction of `rgba` (width x height RGBA8) whose alpha, multiplied by
// `alpha_scale`, passes the glTF alpha test (alpha >= cutoff), estimated with
// 4x4 bilinear sub-samples per texel (edges clamp). An empty or mis-sized
// image has coverage 0.
[[nodiscard]] float alpha_test_coverage(std::span<const std::uint8_t> rgba, std::uint32_t width,
                                        std::uint32_t height, float cutoff,
                                        float alpha_scale = 1.0F);

// The full mip chain of an alpha-tested texture, level 0 = `rgba` unchanged.
// Each level is an area-weighted box filter of the previous one (correct for
// odd sizes; extent max(1, previous / 2) like Vulkan's mip extents) with
// alpha-weighted colour, averaged in linear light when `srgb`. Alpha is the
// plain box-filtered alpha scaled so the level's alpha-test coverage at
// `cutoff` matches level 0's. Returns an empty vector for an empty or
// mis-sized input.
[[nodiscard]] std::vector<MipLevelRgba8>
build_alpha_tested_mips(std::span<const std::uint8_t> rgba, std::uint32_t width,
                        std::uint32_t height, bool srgb, float cutoff);

// The same chain without the coverage step (alpha plainly box-filtered): the
// reference the coverage-preserving chain is compared against.
[[nodiscard]] std::vector<MipLevelRgba8>
build_box_filtered_mips(std::span<const std::uint8_t> rgba, std::uint32_t width,
                        std::uint32_t height, bool srgb);

} // namespace engine::asset
