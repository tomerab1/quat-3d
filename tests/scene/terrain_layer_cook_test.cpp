// quat-cook --terrain-layer (R26.2): a PBR texture set becomes albedo /
// normal / packed KTX2 files of one power-of-two size with full mip chains,
// normals flipped to +Y along +v, packed R roughness / G occlusion / B height
// (normalised), decodable by the engine's own KTX2 path.

#include <cmath>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <vector>

#include <glm/glm.hpp>
#include <gtest/gtest.h>
#include <stb_image_write.h>

#include "engine/asset/ktx2_texture.hpp"
#include "engine/cook/terrain_layer.hpp"
#include "support/expect_ok.hpp"

namespace {

namespace fs = std::filesystem;
namespace a = engine::asset;

constexpr int k_src = 64;

void write_png(const fs::path& p, const std::vector<std::uint8_t>& rgba) {
    std::vector<std::uint8_t> png;
    stbi_write_png_to_func(
        [](void* ctx, void* data, int size) {
            auto* out = static_cast<std::vector<std::uint8_t>*>(ctx);
            const auto* b = static_cast<const std::uint8_t*>(data);
            out->insert(out->end(), b, b + size);
        },
        &png, k_src, k_src, 4, rgba.data(), k_src * 4);
    std::ofstream f(p, std::ios::binary);
    f.write(reinterpret_cast<const char*>(png.data()), static_cast<std::streamsize>(png.size()));
}

std::vector<std::uint8_t> solid(std::uint8_t r, std::uint8_t g, std::uint8_t b) {
    std::vector<std::uint8_t> px(static_cast<std::size_t>(k_src) * k_src * 4);
    for (std::size_t i = 0; i < px.size(); i += 4) {
        px[i] = r;
        px[i + 1] = g;
        px[i + 2] = b;
        px[i + 3] = 255;
    }
    return px;
}

class TerrainLayerCook : public ::testing::Test {
protected:
    fs::path dir;
    void SetUp() override {
        dir = fs::temp_directory_path() /
              ("quat_terrain_layer_" + std::to_string(::testing::UnitTest::GetInstance()->random_seed()));
        fs::remove_all(dir);
        fs::create_directories(dir / "src");
        write_png(dir / "src/albedo.png", solid(200, 120, 60));
        // OpenGL normal tilted toward +Y (up in the image): G above 128.
        write_png(dir / "src/normal.png", solid(128, 200, 220));
        write_png(dir / "src/arm.png", solid(90, 180, 0)); // AO 90, roughness 180
        // Height: a left-to-right ramp (normalised to 0..1 by the cook).
        std::vector<std::uint8_t> h = solid(0, 0, 0);
        for (int y = 0; y < k_src; ++y) {
            for (int x = 0; x < k_src; ++x) {
                const auto v = static_cast<std::uint8_t>(40 + x * 2);
                const std::size_t i = (static_cast<std::size_t>(y) * k_src + x) * 4;
                h[i] = h[i + 1] = h[i + 2] = v;
            }
        }
        write_png(dir / "src/height.png", h);
    }
    void TearDown() override { fs::remove_all(dir); }

    [[nodiscard]] a::TextureData decode(const char* file, a::TextureRole role,
                                        a::TextureColorSpace space) const {
        a::Ktx2DecodeOptions o; // no BC / ASTC caps: RGBA8
        o.role = role;
        o.color_space = space;
        auto t = a::load_ktx2_file(dir / "out" / file, o);
        EXPECT_TRUE(t.has_value()) << (t ? "" : t.error().message);
        return t ? std::move(*t) : a::TextureData{};
    }
};

TEST_F(TerrainLayerCook, WritesThreeMippedSetsOfOneSize) {
    engine::cook::TerrainLayerSources src;
    src.albedo = dir / "src/albedo.png";
    src.normal = dir / "src/normal.png";
    src.arm = dir / "src/arm.png";
    src.height = dir / "src/height.png";
    engine::cook::TerrainLayerCookOptions o;
    o.size = 32;
    o.textures.uastc_rdo = 0.0F; // exact-ish values for the checks below
    ASSERT_OK(engine::cook::cook_terrain_layer(src, dir / "out", o));

    const a::TextureData albedo = decode("albedo.ktx2", a::TextureRole::color, a::TextureColorSpace::srgb);
    const a::TextureData normal = decode("normal.ktx2", a::TextureRole::normal, a::TextureColorSpace::linear);
    const a::TextureData packed = decode("packed.ktx2", a::TextureRole::color, a::TextureColorSpace::linear);
    for (const a::TextureData* t : {&albedo, &normal, &packed}) {
        EXPECT_EQ(t->width, 32U);
        EXPECT_EQ(t->height, 32U);
        EXPECT_EQ(t->levels.size(), 6U); // 32 .. 1
        EXPECT_EQ(t->format, a::TexelFormat::rgba8);
    }
    ASSERT_FALSE(packed.levels.empty());
    const auto texel = [](const a::TextureData& t, std::uint32_t x, std::uint32_t y) {
        const std::byte* p = t.pixels.data() + t.levels[0].offset + (y * t.width + x) * 4;
        return glm::ivec4(std::to_integer<int>(p[0]), std::to_integer<int>(p[1]),
                          std::to_integer<int>(p[2]), std::to_integer<int>(p[3]));
    };
    // Albedo stays the authored sRGB colour.
    EXPECT_NEAR(texel(albedo, 5, 5).r, 200, 6);
    EXPECT_NEAR(texel(albedo, 5, 5).g, 120, 6);
    // Packed: R = ARM roughness (G), G = ARM occlusion (R), B = height ramp
    // normalised: left edge ~0, right edge ~255.
    EXPECT_NEAR(texel(packed, 10, 10).r, 180, 8);
    EXPECT_NEAR(texel(packed, 10, 10).g, 90, 8);
    EXPECT_LT(texel(packed, 0, 16).b, 30);
    EXPECT_GT(texel(packed, 31, 16).b, 220);
    // Normal: the X/Y layout decodes with Y in alpha (RA); the OpenGL +Y
    // tilt became -Y (+Y now points along +v, down the image).
    const glm::ivec4 n = texel(normal, 16, 16);
    EXPECT_LT(n.a, 110) << "Y not flipped (G " << n.g << ", A " << n.a << ")";
}

TEST_F(TerrainLayerCook, RejectsBadInput) {
    engine::cook::TerrainLayerSources src;
    src.albedo = dir / "src/albedo.png";
    EXPECT_FALSE(engine::cook::cook_terrain_layer(src, dir / "out")); // no normal map
    src.normal = dir / "src/normal.png";
    engine::cook::TerrainLayerCookOptions o;
    o.size = 100; // not a power of two
    EXPECT_FALSE(engine::cook::cook_terrain_layer(src, dir / "out", o));
    src.height = dir / "src/missing.png";
    EXPECT_FALSE(engine::cook::cook_terrain_layer(src, dir / "out"));
}

} // namespace
