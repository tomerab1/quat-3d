// KTX2 / Basis Universal loading (R17): files written by the cooker transcode
// to every target format and decode back close to their source, the format
// choice follows the device caps, and malformed files are rejected without
// crashing.

#include <cmath>
#include <cstdint>
#include <cstring>
#include <memory>
#include <random>
#include <vector>

#include <gtest/gtest.h>
#include <ktx.h>

#include "engine/asset/ktx2_texture.hpp"
#include "engine/cook/gltf_cooker.hpp"
#include "support/bc_decode.hpp"
#include "support/expect_ok.hpp"

namespace {

namespace asset = engine::asset;
namespace cook = engine::cook;
using asset::TexelFormat;
using asset::TextureColorSpace;
using asset::TextureRole;
using Caps = engine::rhi::TextureCompressionSupport;

constexpr std::uint32_t k_size = 64;

// Smooth colour gradients with a soft ring: compresses like real albedo.
std::vector<std::byte> colour_image() {
    std::vector<std::byte> px(k_size * k_size * 4);
    for (std::uint32_t y = 0; y < k_size; ++y) {
        for (std::uint32_t x = 0; x < k_size; ++x) {
            const float fx = static_cast<float>(x) / k_size;
            const float fy = static_cast<float>(y) / k_size;
            const float ring = 0.5F + 0.5F * std::sin(10.0F * std::hypot(fx - 0.5F, fy - 0.5F));
            const std::uint8_t c[4] = {static_cast<std::uint8_t>(255 * fx),
                                       static_cast<std::uint8_t>(255 * fy),
                                       static_cast<std::uint8_t>(255 * ring), 255};
            std::memcpy(&px[(y * k_size + x) * 4], c, 4);
        }
    }
    return px;
}

// Tangent-space normals of a gentle bump field, stored as n * 0.5 + 0.5.
std::vector<std::byte> normal_image() {
    std::vector<std::byte> px(k_size * k_size * 4);
    for (std::uint32_t y = 0; y < k_size; ++y) {
        for (std::uint32_t x = 0; x < k_size; ++x) {
            const float nx = 0.4F * std::sin(static_cast<float>(x) * 0.2F);
            const float ny = 0.4F * std::cos(static_cast<float>(y) * 0.15F);
            const float nz = std::sqrt(1.0F - nx * nx - ny * ny);
            const float n[3] = {nx, ny, nz};
            for (int c = 0; c < 3; ++c) {
                px[(y * k_size + x) * 4 + c] =
                    static_cast<std::byte>(std::lround((n[c] * 0.5F + 0.5F) * 255.0F));
            }
            px[(y * k_size + x) * 4 + 3] = std::byte{255};
        }
    }
    return px;
}

// Root-mean-square difference over `channels` of two RGBA8 images.
double rmse(std::span<const std::byte> a, const std::vector<std::uint8_t>& b,
            std::initializer_list<int> channels) {
    double sum = 0;
    std::size_t n = 0;
    for (std::size_t i = 0; i < a.size() / 4; ++i) {
        for (const int c : channels) {
            const double d = static_cast<double>(std::to_integer<std::uint8_t>(a[i * 4 + c])) -
                             static_cast<double>(b[i * 4 + c]);
            sum += d * d;
            ++n;
        }
    }
    return std::sqrt(sum / static_cast<double>(n));
}

// ASTC 4x4 level data -> RGBA8 through libktx's ASTC decoder.
std::vector<std::uint8_t> decode_astc(std::span<const std::byte> blocks, std::uint32_t w,
                                      std::uint32_t h) {
    ktxTextureCreateInfo ci{};
    ci.vkFormat = 157; // VK_FORMAT_ASTC_4x4_UNORM_BLOCK
    ci.baseWidth = w;
    ci.baseHeight = h;
    ci.baseDepth = 1;
    ci.numDimensions = 2;
    ci.numLevels = 1;
    ci.numLayers = 1;
    ci.numFaces = 1;
    ktxTexture2* tex = nullptr;
    if (ktxTexture2_Create(&ci, KTX_TEXTURE_CREATE_ALLOC_STORAGE, &tex) != KTX_SUCCESS) return {};
    std::unique_ptr<ktxTexture2, void (*)(ktxTexture2*)> owner(tex, [](ktxTexture2* t) {
        ktxTexture2_Destroy(t);
    });
    if (ktxTexture_SetImageFromMemory(ktxTexture(tex), 0, 0, 0,
                                      reinterpret_cast<const ktx_uint8_t*>(blocks.data()),
                                      blocks.size()) != KTX_SUCCESS ||
        ktxTexture2_DecodeAstc(tex) != KTX_SUCCESS || tex->dataSize < std::size_t{w} * h * 4) {
        return {};
    }
    return {tex->pData, tex->pData + std::size_t{w} * h * 4};
}

// Level 0 of `data` decoded to RGBA8 on the CPU.
std::vector<std::uint8_t> decode_level0(const asset::TextureData& data) {
    const asset::TextureLevel& l = data.levels.front();
    const std::span<const std::byte> bytes(data.pixels.data() + l.offset, l.size);
    switch (data.format) {
        case TexelFormat::rgba8: {
            const auto* p = reinterpret_cast<const std::uint8_t*>(bytes.data());
            return {p, p + bytes.size()};
        }
        case TexelFormat::astc_4x4: return decode_astc(bytes, l.width, l.height);
        default:                    return quat::test::decode_bc(data.format, bytes, l.width, l.height);
    }
}

std::vector<std::byte> encode(const std::vector<std::byte>& rgba, TextureRole role,
                              TextureColorSpace space, cook::BasisCodec codec) {
    cook::TextureCookOptions o;
    o.color_codec = codec;
    o.normal_codec = codec;
    o.threads = 1;
    auto ktx2 = cook::encode_ktx2(rgba, k_size, k_size, role, space, o);
    EXPECT_TRUE(ktx2.has_value()) << (ktx2 ? "" : ktx2.error().message);
    return ktx2 ? *ktx2 : std::vector<std::byte>{};
}

asset::TextureData decode(const std::vector<std::byte>& file, Caps caps,
                          std::optional<TextureRole> role = std::nullopt) {
    asset::Ktx2DecodeOptions o;
    o.caps = caps;
    o.role = role;
    auto data = asset::decode_ktx2(file, o);
    EXPECT_TRUE(data.has_value()) << (data ? "" : data.error().message);
    return data ? std::move(*data) : asset::TextureData{};
}

constexpr Caps k_bc{true, false, false};
constexpr Caps k_astc{false, true, false};
constexpr Caps k_none{};

} // namespace

TEST(Ktx2Format, SelectionFollowsDeviceCaps) {
    const auto pick = [](Caps caps, TextureRole role, TextureColorSpace cs, bool xy) {
        return asset::select_transcode_format(caps, role, cs, xy);
    };
    const auto lin = TextureColorSpace::linear;
    const auto srgb = TextureColorSpace::srgb;
    EXPECT_EQ(pick(k_bc, TextureRole::color, srgb, false), TexelFormat::bc7);
    EXPECT_EQ(pick(k_bc, TextureRole::normal, lin, true), TexelFormat::bc5);
    EXPECT_EQ(pick(k_bc, TextureRole::normal, lin, false), TexelFormat::bc7); // RGB normals
    EXPECT_EQ(pick(k_bc, TextureRole::single_channel, lin, false), TexelFormat::bc4);
    EXPECT_EQ(pick(k_bc, TextureRole::single_channel, srgb, false), TexelFormat::bc7);
    EXPECT_EQ(pick(k_astc, TextureRole::normal, lin, true), TexelFormat::astc_4x4);
    EXPECT_EQ(pick(Caps{true, true, true}, TextureRole::color, srgb, false), TexelFormat::astc_4x4);
    EXPECT_EQ(pick(Caps{true, true, false}, TextureRole::color, srgb, false), TexelFormat::bc7);
    EXPECT_EQ(pick(k_none, TextureRole::color, srgb, false), TexelFormat::rgba8);
}

TEST(Ktx2Decode, UastcColourTranscodesCloseToTheSourceForEveryTarget) {
    const std::vector<std::byte> src = colour_image();
    const auto file = encode(src, TextureRole::color, TextureColorSpace::srgb, cook::BasisCodec::uastc);
    ASSERT_FALSE(file.empty());
    for (const auto& [caps, format] : {std::pair{k_bc, TexelFormat::bc7},
                                       std::pair{k_astc, TexelFormat::astc_4x4},
                                       std::pair{k_none, TexelFormat::rgba8}}) {
        const asset::TextureData data = decode(file, caps);
        ASSERT_EQ(data.format, format);
        EXPECT_EQ(data.color_space, TextureColorSpace::srgb);
        ASSERT_EQ(data.levels.size(), 7U); // 64 -> 1
        EXPECT_EQ(data.levels.back().width, 1U);
        const auto px = decode_level0(data);
        ASSERT_EQ(px.size(), src.size()) << static_cast<int>(format);
        EXPECT_LT(rmse(src, px, {0, 1, 2}), 4.0) << static_cast<int>(format);
    }
}

TEST(Ktx2Decode, Etc1sColourTranscodesWithinItsLowerQuality) {
    const std::vector<std::byte> src = colour_image();
    const auto file = encode(src, TextureRole::color, TextureColorSpace::srgb, cook::BasisCodec::etc1s);
    ASSERT_FALSE(file.empty());
    // ETC1S is the small, lossy codec: the RGBA8 transcode carries its error,
    // and the block formats must reproduce that transcode closely.
    const auto reference = decode_level0(decode(file, k_none));
    ASSERT_EQ(reference.size(), src.size());
    EXPECT_LT(rmse(src, reference, {0, 1, 2}), 14.0);
    std::vector<std::byte> ref_bytes(reference.size());
    std::memcpy(ref_bytes.data(), reference.data(), reference.size());
    for (const Caps caps : {k_bc, k_astc}) {
        const auto px = decode_level0(decode(file, caps));
        ASSERT_EQ(px.size(), src.size());
        EXPECT_LT(rmse(ref_bytes, px, {0, 1, 2}), 2.0);
    }
}

TEST(Ktx2Decode, NormalMapsUseBc5OrSwizzleXyIntoRg) {
    const std::vector<std::byte> src = normal_image();
    const auto file = encode(src, TextureRole::normal, TextureColorSpace::linear, cook::BasisCodec::uastc);
    ASSERT_FALSE(file.empty());
    const asset::TextureData bc5 = decode(file, k_bc);
    ASSERT_EQ(bc5.format, TexelFormat::bc5);
    EXPECT_EQ(bc5.swizzle, asset::TextureSwizzle::identity);
    EXPECT_LT(rmse(src, decode_level0(bc5), {0, 1}), 3.0);

    // Without BC5 the X-in-RGB / Y-in-alpha layout is routed to RG by the view.
    const asset::TextureData rgba = decode(file, k_none);
    ASSERT_EQ(rgba.format, TexelFormat::rgba8);
    EXPECT_EQ(rgba.swizzle, asset::TextureSwizzle::ra01);
    std::vector<std::uint8_t> px = decode_level0(rgba);
    for (std::size_t i = 0; i < px.size() / 4; ++i) px[i * 4 + 1] = px[i * 4 + 3]; // apply ra01
    EXPECT_LT(rmse(src, px, {0, 1}), 3.0);
}

TEST(Ktx2Decode, SingleChannelDataUsesBc4) {
    const std::vector<std::byte> src = colour_image();
    const auto file = encode(src, TextureRole::single_channel, TextureColorSpace::linear,
                             cook::BasisCodec::uastc);
    ASSERT_FALSE(file.empty());
    const asset::TextureData data = decode(file, k_bc);
    ASSERT_EQ(data.format, TexelFormat::bc4);
    EXPECT_EQ(data.swizzle, asset::TextureSwizzle::rrr1);
    EXPECT_LT(rmse(src, decode_level0(data), {0}), 3.0);
}

TEST(Ktx2Decode, LooseFileRoleComesFromTheDescriptor) {
    const auto file = encode(normal_image(), TextureRole::normal, TextureColorSpace::linear,
                             cook::BasisCodec::etc1s);
    ASSERT_FALSE(file.empty());
    const asset::TextureData data = decode(file, k_bc); // no role given
    EXPECT_EQ(data.format, TexelFormat::bc5);
    EXPECT_EQ(data.color_space, TextureColorSpace::linear);
}

namespace {

std::vector<std::byte> valid_file() {
    return encode(colour_image(), TextureRole::color, TextureColorSpace::srgb, cook::BasisCodec::uastc);
}

void put_u32(std::vector<std::byte>& f, std::size_t at, std::uint32_t v) {
    std::memcpy(f.data() + at, &v, 4);
}
void put_u64(std::vector<std::byte>& f, std::size_t at, std::uint64_t v) {
    std::memcpy(f.data() + at, &v, 8);
}

} // namespace

TEST(Ktx2Decode, MalformedHeadersAreRejected) {
    const std::vector<std::byte> good = valid_file();
    ASSERT_GT(good.size(), 200U);
    ASSERT_OK(asset::validate_ktx2(good));
    const auto rejects = [&](auto mutate, const char* what) {
        std::vector<std::byte> f = good;
        mutate(f);
        EXPECT_ERR(asset::decode_ktx2(f, asset::Ktx2DecodeOptions{})) << what;
    };
    rejects([](auto& f) { f.resize(60); }, "truncated header");
    rejects([](auto& f) { f.resize(f.size() / 2); }, "truncated data");
    rejects([](auto& f) { f[1] = std::byte{'X'}; }, "bad identifier");
    rejects([](auto& f) { put_u32(f, 20, 1U << 20); }, "absurd width");
    rejects([](auto& f) { put_u32(f, 24, 0); }, "1D texture");
    rejects([](auto& f) { put_u32(f, 28, 4); }, "3D texture");
    rejects([](auto& f) { put_u32(f, 36, 6); }, "cubemap");
    rejects([](auto& f) { put_u32(f, 40, 40); }, "too many levels");
    rejects([](auto& f) { put_u32(f, 44, 3); }, "zlib supercompression");
    rejects([](auto& f) { put_u32(f, 12, 1000); }, "unknown VkFormat");
    rejects([](auto& f) { put_u32(f, 48, 0xFFFFFF00U); }, "DFD outside the file");
    rejects([](auto& f) { put_u64(f, 80, ~0ULL - 4); }, "level offset overflow");
    rejects([](auto& f) { put_u64(f, 96, 1ULL << 40); }, "zstd decompression bomb");
}

TEST(Ktx2Decode, RejectsTexturesAboveTheConfiguredLimits) {
    const std::vector<std::byte> good = valid_file();
    asset::Ktx2Limits limits;
    limits.max_dimension = 32;
    EXPECT_ERR(asset::validate_ktx2(good, limits));
    limits = {};
    limits.max_file_bytes = 100;
    EXPECT_ERR(asset::validate_ktx2(good, limits));
}

// Random byte corruption of a valid file must only ever produce an error or a
// texture, never a crash (run under ASan/valgrind to catch silent reads).
TEST(Ktx2Decode, CorruptedFilesNeverCrash) {
    const std::vector<std::byte> good = valid_file();
    ASSERT_FALSE(good.empty());
    std::mt19937 rng(1234);
    std::uniform_int_distribution<std::size_t> pos(0, good.size() - 1);
    std::uniform_int_distribution<int> byte(0, 255);
    for (int round = 0; round < 300; ++round) {
        std::vector<std::byte> f = good;
        const int flips = 1 + round % 8;
        for (int i = 0; i < flips; ++i) {
            // Bias towards the header / index where the structure lives.
            const std::size_t at = round % 2 == 0 ? pos(rng) % 160 : pos(rng);
            f[at] = static_cast<std::byte>(byte(rng));
        }
        auto r = asset::decode_ktx2(f, asset::Ktx2DecodeOptions{k_bc, {}, {}, {}});
        if (r) {
            EXPECT_FALSE(r->levels.empty());
        }
    }
}
