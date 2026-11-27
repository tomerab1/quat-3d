// Guarded PNG / JPEG decoding (#31, R17): headers are checked against the
// texture limits before any pixel memory is allocated, only PNG and JPEG are
// accepted, and one load's images share a decoded-byte budget.

#include <cstdint>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <string>
#include <vector>

#include <fastgltf/core.hpp>
#include <gtest/gtest.h>
#include <stb_image_write.h>

#include "engine/asset/image_decode.hpp"
#include "scene/gltf/internal.hpp"
#include "support/expect_ok.hpp"
#include "support/temp_dir.hpp"

namespace {

namespace asset = engine::asset;

void put_be32(std::vector<std::uint8_t>& out, std::uint32_t v) {
    for (int shift = 24; shift >= 0; shift -= 8) out.push_back(static_cast<std::uint8_t>(v >> shift));
}

// A PNG that is only a signature, an IHDR for a 1-bit greyscale image of
// `w` x `h`, an empty IDAT and IEND: tiny on disk, enormous when decoded.
std::vector<std::byte> png_header_only(std::uint32_t w, std::uint32_t h) {
    std::vector<std::uint8_t> png{0x89, 'P', 'N', 'G', '\r', '\n', 0x1A, '\n'};
    put_be32(png, 13);
    for (const char c : std::string("IHDR")) png.push_back(static_cast<std::uint8_t>(c));
    put_be32(png, w);
    put_be32(png, h);
    png.insert(png.end(), {1, 0, 0, 0, 0}); // bit depth 1, greyscale, no interlace
    put_be32(png, 0);                       // CRC (not checked by stb_image)
    for (const char* chunk : {"IDAT", "IEND"}) {
        put_be32(png, 0);
        for (int i = 0; i < 4; ++i) png.push_back(static_cast<std::uint8_t>(chunk[i]));
        put_be32(png, 0);
    }
    std::vector<std::byte> out(png.size());
    std::memcpy(out.data(), png.data(), png.size());
    return out;
}

// A real `size` x `size` RGBA PNG with a known first pixel.
std::vector<std::byte> small_png(int size) {
    std::vector<std::uint8_t> rgba(static_cast<std::size_t>(size * size * 4), 200);
    rgba[0] = 10;
    rgba[1] = 20;
    rgba[2] = 30;
    std::vector<std::byte> png;
    stbi_write_png_to_func(
        [](void* ctx, void* data, int n) {
            auto* out = static_cast<std::vector<std::byte>*>(ctx);
            const auto* p = static_cast<const std::byte*>(data);
            out->insert(out->end(), p, p + n);
        },
        &png, size, size, 4, rgba.data(), size * 4);
    return png;
}

bool mentions(const std::string& message, const char* text) {
    return message.find(text) != std::string::npos;
}

} // namespace

TEST(ImageDecode, NormalPngLoadsWithoutACopy) {
    auto img = asset::decode_png_jpeg(small_png(8), asset::TextureColorSpace::srgb);
    ASSERT_OK(img);
    EXPECT_EQ(img->width, 8U);
    EXPECT_EQ(img->height, 8U);
    ASSERT_EQ(img->pixels.size(), 8U * 8U * 4U);
    EXPECT_EQ(std::to_integer<int>(img->pixels[0]), 10);
    EXPECT_EQ(std::to_integer<int>(img->pixels[2]), 30);
    EXPECT_EQ(img->color_space, asset::TextureColorSpace::srgb);
}

// Issue #31: a 48 KB 1-bit 20000 x 20000 PNG used to decode to 1.6 GB.
TEST(ImageDecode, HugePngHeaderIsRejectedBeforeAllocating) {
    const std::vector<std::byte> png = png_header_only(20000, 20000);
    asset::DecodeBudget budget(asset::TextureLimits{}.max_total_decoded_bytes);
    auto img = asset::decode_png_jpeg(png, asset::TextureColorSpace::linear, {}, &budget);
    ASSERT_ERR(img);
    EXPECT_TRUE(mentions(img.error().message, "20000x20000")) << img.error().message;
    EXPECT_TRUE(mentions(img.error().message, "8192")) << img.error().message;
    EXPECT_EQ(budget.used(), 0U); // nothing was claimed, so nothing was allocated

    // With the engine cap raised, stb's own STBI_MAX_DIMENSIONS still refuses.
    asset::TextureLimits raised;
    raised.max_dimension = 65536;
    raised.max_decoded_bytes = ~0ULL;
    EXPECT_ERR(asset::decode_png_jpeg(png, asset::TextureColorSpace::linear, raised));
}

TEST(ImageDecode, DecodedByteLimitAppliesBelowTheDimensionCap) {
    asset::TextureLimits limits;
    limits.max_dimension = 16384;
    auto img = asset::decode_png_jpeg(png_header_only(12000, 12000),
                                      asset::TextureColorSpace::linear, limits);
    ASSERT_ERR(img);
    EXPECT_TRUE(mentions(img.error().message, "per-image limit")) << img.error().message;
}

TEST(ImageDecode, DeviceMaxDimensionLowersTheCap) {
    engine::rhi::TextureCaps caps;
    caps.max_dimension_2d = 4;
    const asset::TextureLimits limits = asset::clamp_to_device({}, caps);
    EXPECT_EQ(limits.max_dimension, 4U);
    EXPECT_ERR(asset::decode_png_jpeg(small_png(8), asset::TextureColorSpace::linear, limits));
}

TEST(ImageDecode, OnlyPngAndJpegAreAccepted) {
    // An uncompressed 2x2 TGA: valid for a full stb_image, refused here.
    std::vector<std::uint8_t> tga{0, 0, 2, 0, 0, 0, 0, 0, 0, 0, 0, 0, 2, 0, 2, 0, 32, 8};
    tga.resize(tga.size() + 16, 0x7F);
    std::vector<std::byte> bytes(tga.size());
    std::memcpy(bytes.data(), tga.data(), tga.size());
    auto img = asset::decode_png_jpeg(bytes, asset::TextureColorSpace::linear);
    ASSERT_ERR(img);
    EXPECT_TRUE(mentions(img.error().message, "PNG and JPEG only")) << img.error().message;
}

TEST(ImageDecode, BudgetIsSharedAcrossImages) {
    const std::vector<std::byte> png = small_png(8);
    asset::DecodeBudget budget(2 * 8 * 8 * 4);
    EXPECT_OK(asset::decode_png_jpeg(png, asset::TextureColorSpace::linear, {}, &budget));
    EXPECT_OK(asset::decode_png_jpeg(png, asset::TextureColorSpace::linear, {}, &budget));
    auto third = asset::decode_png_jpeg(png, asset::TextureColorSpace::linear, {}, &budget);
    ASSERT_ERR(third);
    EXPECT_TRUE(mentions(third.error().message, "budget")) << third.error().message;
}

// The glTF loader decodes a file's images in parallel under one budget.
TEST(ImageDecode, GltfLoadStopsAtTheTotalBudget) {
    quat::test::TempDir dir("image-budget");
    const std::vector<std::byte> png = small_png(16);
    std::string images;
    std::string textures;
    std::string materials;
    for (int i = 0; i < 3; ++i) {
        const std::string name = "img" + std::to_string(i) + ".png";
        std::ofstream(dir.path() / name, std::ios::binary)
            .write(reinterpret_cast<const char*>(png.data()), static_cast<std::streamsize>(png.size()));
        const std::string sep = i == 0 ? "" : ",";
        images += sep + R"({"uri":")" + name + R"("})";
        textures += sep + R"({"source":)" + std::to_string(i) + "}";
        materials += sep + R"({"pbrMetallicRoughness":{"baseColorTexture":{"index":)" +
                     std::to_string(i) + "}}}";
    }
    const std::string json = R"({"asset":{"version":"2.0"},"images":[)" + images +
                             R"(],"textures":[)" + textures + R"(],"materials":[)" + materials +
                             "]}";
    std::ofstream(dir.path() / "three.gltf") << json;

    auto data = fastgltf::GltfDataBuffer::FromPath(dir.path() / "three.gltf");
    ASSERT_EQ(data.error(), fastgltf::Error::None);
    auto parsed = engine::scene::gltf_detail::parse_gltf(data.get(), dir.path());
    ASSERT_OK(parsed);
    EXPECT_OK(engine::scene::gltf_detail::extract_textures(*parsed));

    asset::TextureLimits tight;
    tight.max_total_decoded_bytes = 2 * 16 * 16 * 4 + 100; // room for two of three
    auto over = engine::scene::gltf_detail::extract_textures(*parsed, {}, tight);
    ASSERT_ERR(over);
    EXPECT_TRUE(mentions(over.error().message, "budget")) << over.error().message;
}
