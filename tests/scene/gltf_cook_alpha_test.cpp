// Cooking an alpha-tested texture (R12.11, issue #35): the KTX2 mip chain
// quat-cook writes for a glTF MASK material's base colour keeps the alpha-
// test coverage of level 0, where the same image cooked for an OPAQUE
// material (plain box filter) loses its one-texel blades a few mips down.

#include <cmath>
#include <cstdint>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <sstream>
#include <string>
#include <vector>

#include <gtest/gtest.h>
#include <stb_image_write.h>

#include "engine/asset/alpha_coverage.hpp"
#include "engine/cook/gltf_cooker.hpp"
#include "engine/scene/gltf_loader.hpp"
#include "support/expect_ok.hpp"
#include "support/temp_dir.hpp"

namespace {

namespace fs = std::filesystem;
constexpr int k_tex = 128;

void write_file(const fs::path& p, const void* data, std::size_t size) {
    std::ofstream f(p, std::ios::binary);
    f.write(static_cast<const char*>(data), static_cast<std::streamsize>(size));
}

// Green one-texel blades every four texels plus a sparse speckle, on a
// transparent background (the CPU reference test's foliage-like mask).
void write_blades_png(const fs::path& p) {
    std::vector<std::uint8_t> rgba(static_cast<std::size_t>(k_tex) * k_tex * 4);
    for (int y = 0; y < k_tex; ++y) {
        for (int x = 0; x < k_tex; ++x) {
            std::uint8_t* px = &rgba[(static_cast<std::size_t>(y) * k_tex + x) * 4];
            px[0] = 40;
            px[1] = 160;
            px[2] = 40;
            const auto speckle = (static_cast<std::uint32_t>(x) * 73856093U) ^
                                 (static_cast<std::uint32_t>(y) * 19349663U);
            px[3] = (x % 4 == 0 || speckle % 11 == 0) ? 255 : 0;
        }
    }
    std::vector<std::uint8_t> png;
    stbi_write_png_to_func(
        [](void* ctx, void* data, int size) {
            auto* out = static_cast<std::vector<std::uint8_t>*>(ctx);
            out->insert(out->end(), static_cast<std::uint8_t*>(data),
                        static_cast<std::uint8_t*>(data) + size);
        },
        &png, k_tex, k_tex, 4, rgba.data(), k_tex * 4);
    write_file(p, png.data(), png.size());
}

// One textured triangle whose material uses `alpha_mode`.
fs::path write_gltf(const fs::path& dir, const char* alpha_mode) {
    const std::vector<float> pos{0, 0, 0, 1, 0, 0, 0, 1, 0};
    const std::vector<float> uv{0, 0, 1, 0, 0, 1};
    const std::vector<std::uint16_t> idx{0, 1, 2, 0};
    std::vector<std::uint8_t> bin(36 + 24 + 8);
    std::memcpy(bin.data(), pos.data(), 36);
    std::memcpy(bin.data() + 36, uv.data(), 24);
    std::memcpy(bin.data() + 60, idx.data(), 6);
    write_file(dir / "tri.bin", bin.data(), bin.size());
    write_blades_png(dir / "blades.png");
    std::ostringstream j;
    j << R"({"asset":{"version":"2.0"},"scene":0,"scenes":[{"nodes":[0]}],"nodes":[{"mesh":0}],)"
      << R"("meshes":[{"primitives":[{"attributes":{"POSITION":0,"TEXCOORD_0":1},"indices":2,"material":0}]}],)"
      << R"("materials":[{"pbrMetallicRoughness":{"baseColorTexture":{"index":0}},"alphaMode":")"
      << alpha_mode << R"(","alphaCutoff":0.5}],)"
      << R"("textures":[{"source":0}],"images":[{"uri":"blades.png"}],)"
      << R"("buffers":[{"uri":"tri.bin","byteLength":)" << bin.size() << "}],"
      << R"("bufferViews":[{"buffer":0,"byteOffset":0,"byteLength":36},)"
      << R"({"buffer":0,"byteOffset":36,"byteLength":24},{"buffer":0,"byteOffset":60,"byteLength":6}],)"
      << R"("accessors":[)"
      << R"({"bufferView":0,"componentType":5126,"count":3,"type":"VEC3","min":[0,0,0],"max":[1,1,0]},)"
      << R"({"bufferView":1,"componentType":5126,"count":3,"type":"VEC2"},)"
      << R"({"bufferView":2,"componentType":5123,"count":3,"type":"SCALAR"}]})";
    const fs::path out = dir / (std::string("tri_") + alpha_mode + ".gltf");
    const std::string text = j.str();
    write_file(out, text.data(), text.size());
    return out;
}

// Alpha-test coverage (cutoff 0.5) of each mip of the cooked base colour.
std::vector<float> cooked_coverage(const char* alpha_mode) {
    quat::test::TempDir dir("gltf-cook-alpha");
    const fs::path src = write_gltf(dir.path(), alpha_mode);
    const fs::path out = dir.path() / "cooked.glb";
    engine::cook::CookOptions options;
    options.textures.threads = 1;
    auto stats = engine::cook::cook_gltf(src, out, options);
    EXPECT_OK(stats);
    std::vector<float> coverage;
    if (!stats) return coverage;
    EXPECT_EQ(stats->images_encoded, 1U);
    // No device caps: the KTX2 image decodes to RGBA8 with its mip chain.
    auto textures = engine::scene::GltfLoader::load_texture_data(out);
    EXPECT_OK(textures);
    if (!textures || textures->size() != 1) return coverage;
    const engine::asset::TextureData& t = (*textures)[0];
    for (const engine::asset::TextureLevel& level : t.levels) {
        const std::span<const std::uint8_t> px(
            reinterpret_cast<const std::uint8_t*>(t.pixels.data()) + level.offset, level.size);
        coverage.push_back(
            engine::asset::alpha_test_coverage(px, level.width, level.height, 0.5F));
    }
    return coverage;
}

TEST(GltfCookAlpha, MaskTexturesKeepTheirCoverageWhenCooked) {
    const std::vector<float> mask = cooked_coverage("MASK");
    const std::vector<float> opaque = cooked_coverage("OPAQUE");
    ASSERT_GE(mask.size(), 5U);
    ASSERT_EQ(mask.size(), opaque.size());
    const float target = mask[0];
    ASSERT_GT(target, 0.15F);
    // Block compression is lossy: a looser bound than the CPU reference test.
    for (std::size_t i = 1; i < 5; ++i) {
        EXPECT_NEAR(mask[i], target, 0.08F) << "MASK level " << i;
    }
    // Box-filtered (OPAQUE) blades drop below the cutoff by level 2.
    EXPECT_LT(opaque[2], 0.5F * target);
}

} // namespace
