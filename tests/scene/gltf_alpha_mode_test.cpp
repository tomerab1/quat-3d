// glTF alphaMode (R12.11, issue #35): MASK sets material_alpha_mask (BLEND
// stays on the forward path, OPAQUE ignores alphaCutoff), and the base-colour
// image of a MASK material gets coverage-preserving mips at its cutoff.

#include <cstddef>
#include <span>
#include <string>

#include <gtest/gtest.h>

#include "engine/asset/material_asset.hpp"
#include "engine/scene/gltf_loader.hpp"
#include "support/expect_ok.hpp"

namespace {

namespace a = engine::asset;
namespace s = engine::scene;

// Images: 0 = base colour of the MASK and BLEND materials, 1 = base colour of
// the OPAQUE material (a 2x2 RGBA PNG with two transparent texels).
constexpr const char* k_png =
    "data:image/png;base64,iVBORw0KGgoAAAANSUhEUgAAAAIAAAACCAYAAABytg0kAAAAGElEQVR4nAXBAQEAAAjDIG7/"
    "zhNWhR2GHj8GBQG9doDRAAAAAElFTkSuQmCC";

std::string gltf() {
    return std::string(R"({"asset":{"version":"2.0"},"images":[{"uri":")") + k_png +
           R"("},{"uri":")" + k_png +
           R"("}],"textures":[{"source":0},{"source":1}],"materials":[)"
           R"({"pbrMetallicRoughness":{"baseColorTexture":{"index":0}},)"
           R"("alphaMode":"MASK","alphaCutoff":0.3,"doubleSided":true},)"
           R"({"pbrMetallicRoughness":{"baseColorTexture":{"index":0}},"alphaMode":"BLEND"},)"
           R"({"pbrMetallicRoughness":{"baseColorTexture":{"index":1}},"alphaCutoff":0.9}]})";
}

std::span<const std::byte> bytes(const std::string& text) {
    return {reinterpret_cast<const std::byte*>(text.data()), text.size()};
}

TEST(GltfAlphaMode, MaskSetsTheAlphaMaskFlag) {
    const std::string text = gltf();
    auto mats = s::GltfLoader::load_material_data_from_memory(bytes(text), ".");
    ASSERT_OK(mats);
    ASSERT_EQ(mats->size(), 3U);
    const auto& mask = (*mats)[0].params;
    EXPECT_NE(mask.flags & a::material_alpha_mask, 0U);
    EXPECT_EQ(mask.flags & a::material_blend, 0U);
    EXPECT_NE(mask.flags & a::material_double_sided, 0U);
    EXPECT_FLOAT_EQ(mask.alpha_cutoff, 0.3F);
    const auto& blend = (*mats)[1].params;
    EXPECT_NE(blend.flags & a::material_blend, 0U);
    EXPECT_EQ(blend.flags & a::material_alpha_mask, 0U);
    const auto& opaque = (*mats)[2].params;
    EXPECT_EQ(opaque.flags & (a::material_blend | a::material_alpha_mask), 0U);
}

// A MASK material's base colour arrives with its coverage-preserving mip
// chain built on the CPU; other images leave their mips to the upload.
TEST(GltfAlphaMode, MaskBaseColourGetsCoverageMips) {
    const std::string text = gltf();
    auto images = s::GltfLoader::load_texture_data_from_memory(bytes(text), ".");
    ASSERT_OK(images);
    ASSERT_EQ(images->size(), 2U);
    const a::TextureData& mask = (*images)[0];
    ASSERT_EQ(mask.levels.size(), 2U); // 2x2 -> 1x1
    EXPECT_EQ(mask.levels[1].width, 1U);
    EXPECT_EQ(mask.levels[1].height, 1U);
    EXPECT_EQ(mask.levels[0].size, 16U);
    EXPECT_EQ(mask.pixels.size(), 20U);
    EXPECT_TRUE((*images)[1].levels.empty()); // OPAQUE's image: blitted on upload
}

} // namespace
