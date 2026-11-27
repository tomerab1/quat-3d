#pragma once

// CPU-side texture description shared by the loaders, the GPU upload and the
// offline cooker (no graphics-API types).

#include <cstddef>
#include <cstdint>
#include <memory>
#include <span>
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

// Owned texel storage that can adopt a decoder's own allocation (stb_image's
// output) without copying. Move-only; empty when default-constructed or when
// allocate() could not get the memory.
// Deleter of PixelBuffer storage: the allocator's own free function.
struct PixelRelease {
    void (*fn)(void*) = nullptr;
    void operator()(std::byte* p) const noexcept {
        if (fn != nullptr) fn(p);
    }
};

class PixelBuffer {
public:
    using FreeFn = void (*)(void*);

    PixelBuffer() = default;
    [[nodiscard]] static PixelBuffer allocate(std::size_t size);
    // Takes ownership of `data` (`size` bytes), released with `free_fn`.
    [[nodiscard]] static PixelBuffer adopt(void* data, std::size_t size, FreeFn free_fn) noexcept;

    [[nodiscard]] std::byte* data() noexcept { return data_.get(); }
    [[nodiscard]] const std::byte* data() const noexcept { return data_.get(); }
    [[nodiscard]] std::size_t size() const noexcept { return size_; }
    [[nodiscard]] bool empty() const noexcept { return size_ == 0; }
    [[nodiscard]] std::byte& operator[](std::size_t i) noexcept { return data_.get()[i]; }
    [[nodiscard]] std::byte operator[](std::size_t i) const noexcept { return data_.get()[i]; }
    [[nodiscard]] const std::byte* begin() const noexcept { return data(); }
    [[nodiscard]] const std::byte* end() const noexcept { return data() + size_; }
    [[nodiscard]] std::span<const std::byte> span() const noexcept { return {data(), size_}; }

private:
    std::unique_ptr<std::byte, PixelRelease> data_;
    std::size_t size_ = 0;
};

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
    PixelBuffer               pixels;
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
