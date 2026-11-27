#pragma once

// CPU-side texture description shared by the loaders, the GPU upload and the
// offline cooker (no graphics-API types).

#include <cstddef>
#include <cstdint>
#include <vector>

namespace engine::asset {

// How a glTF texture's bytes should be interpreted. Base colour and emissive are
// authored in sRGB; normal, metallic-roughness and occlusion are linear data.
enum class TextureColorSpace : std::uint8_t { srgb, linear };

// Texel encoding of CPU-side texture data. rgba8 is 4 bytes per texel; the
// block formats are 4x4 blocks of 16 bytes (bc7, bc5, astc_4x4) or 8 bytes
// (bc4).
enum class TexelFormat : std::uint8_t { rgba8, bc7, bc5, bc4, astc_4x4 };

// What a texture feeds, which decides the compressed format it is transcoded
// to: colour (any RGBA data), a tangent-space normal map (only X/Y are read;
// Z is reconstructed in the shader), or single-channel data read from R.
enum class TextureRole : std::uint8_t { color, normal, single_channel };

// Channel routing applied by the image view so every format reads the same in
// shaders: rrr1 widens a single-channel format to grey, ra01 moves a normal
// map stored as X in RGB and Y in alpha (the Basis normal-map layout) to RG.
enum class TextureSwizzle : std::uint8_t { identity, rrr1, ra01 };

// One mip level inside TextureData::pixels.
struct TextureLevel {
    std::uint64_t offset = 0;
    std::uint64_t size   = 0;
    std::uint32_t width  = 0;
    std::uint32_t height = 0;
};

// CPU-side texture ready for upload. With `levels` empty, `pixels` is one
// tightly packed RGBA8 image whose mip chain is generated on upload;
// otherwise it holds every listed level (level 0 first) in `format`.
struct TextureData {
    std::vector<std::byte>    pixels;
    std::uint32_t             width  = 0;
    std::uint32_t             height = 0;
    TextureColorSpace         color_space = TextureColorSpace::linear;
    TexelFormat               format  = TexelFormat::rgba8;
    TextureSwizzle            swizzle = TextureSwizzle::identity;
    std::vector<TextureLevel> levels;
};

// Bytes of one level of `format` at `width` x `height` texels.
[[nodiscard]] std::uint64_t texel_level_size(TexelFormat format, std::uint32_t width,
                                             std::uint32_t height) noexcept;

} // namespace engine::asset
