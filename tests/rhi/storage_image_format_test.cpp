// Storage images declare their format (S1 audit A10): an RWTexture without
// [format(...)] needs the optional shaderStorageImageWriteWithoutFormat
// feature, which the engine never enables. The SPIR-V walk finds the declared
// formats; pipeline creation refuses undeclared or unsupported ones.

#include <array>
#include <cstdint>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <string>
#include <vector>

#include <gtest/gtest.h>
#include <vulkan/vulkan.h>

#include "engine/rhi/device.hpp"
#include "engine/rhi/shader_reflection.hpp"
#include "support/expect_ok.hpp"
#include "support/gpu_context.hpp"

namespace {

using engine::rhi::spirv_storage_image_formats;

// SPIR-V header + one OpTypeImage per (dim, sampled, format).
struct ImageDecl {
    std::uint32_t dim;     // 1 = 2D, 5 = Buffer, 6 = SubpassData
    std::uint32_t sampled; // 1 = sampled, 2 = storage
    std::uint32_t format;  // SPIR-V ImageFormat
};
std::vector<std::uint32_t> module_with(const std::vector<ImageDecl>& images) {
    std::vector<std::uint32_t> words{0x07230203U, 0x00010600U, 0U, 100U, 0U};
    words.push_back((3U << 16U) | 22U); // OpTypeFloat %1 32
    words.push_back(1U);
    words.push_back(32U);
    std::uint32_t id = 2;
    for (const ImageDecl& img : images) {
        words.push_back((9U << 16U) | 25U); // OpTypeImage, 9 words
        words.insert(words.end(), {id++, 1U, img.dim, 0U, 0U, 0U, img.sampled, img.format});
    }
    return words;
}

TEST(StorageImageFormats, SpirvWalkReportsStorageImagesOnly) {
    const auto words = module_with({
        {1, 2, 2},  // storage 2D rgba16f
        {1, 2, 0},  // storage 2D, no format
        {1, 1, 0},  // sampled texture: not a storage image
        {5, 2, 4},  // storage texel buffer: not an image
        {6, 2, 0},  // subpass input
        {1, 2, 4},  // storage 2D rgba8
    });
    const std::vector<VkFormat> formats = spirv_storage_image_formats(words);
    const std::vector<VkFormat> expected{VK_FORMAT_R16G16B16A16_SFLOAT, VK_FORMAT_UNDEFINED,
                                         VK_FORMAT_R8G8B8A8_UNORM};
    EXPECT_EQ(formats, expected);
}

TEST(StorageImageFormats, MalformedModulesYieldNothing) {
    EXPECT_TRUE(spirv_storage_image_formats({}).empty());
    std::vector<std::uint32_t> bad_magic = module_with({{1, 2, 2}});
    bad_magic[0] = 0xDEADBEEFU;
    EXPECT_TRUE(spirv_storage_image_formats(bad_magic).empty());
    std::vector<std::uint32_t> truncated = module_with({{1, 2, 2}});
    truncated.pop_back();
    EXPECT_TRUE(spirv_storage_image_formats(truncated).empty());
}

// Every cooked shader declares a format on each storage image it uses.
TEST(StorageImageFormats, EveryCookedShaderDeclaresItsStorageImageFormats) {
    const std::filesystem::path dir = QUAT_COOKED_SHADER_DIR;
    std::error_code ec;
    if (!std::filesystem::is_directory(dir, ec)) GTEST_SKIP() << "no cooked shaders at " << dir;
    int checked = 0;
    for (const auto& entry : std::filesystem::directory_iterator(dir, ec)) {
        if (entry.path().extension() != ".spv") continue;
        std::ifstream in(entry.path(), std::ios::binary);
        std::vector<char> bytes((std::istreambuf_iterator<char>(in)), {});
        ASSERT_EQ(bytes.size() % 4, 0U) << entry.path();
        std::vector<std::uint32_t> words(bytes.size() / 4);
        std::memcpy(words.data(), bytes.data(), bytes.size());
        for (const VkFormat f : spirv_storage_image_formats(words)) {
            EXPECT_NE(f, VK_FORMAT_UNDEFINED)
                << entry.path().filename() << " declares a storage image without [format(...)]";
        }
        ++checked;
    }
    EXPECT_GT(checked, 0);
}

TEST(GpuStorageImageFormats, DeviceAcceptsDeclaredFormatsAndRefusesUndeclaredOnes) {
    QUAT_REQUIRE_GPU(g);
    // Both are mandatory storage formats in Vulkan.
    const std::array<VkFormat, 2> ok{VK_FORMAT_R16G16B16A16_SFLOAT, VK_FORMAT_R8G8B8A8_UNORM};
    EXPECT_OK(g->device.check_storage_image_formats(ok, "test"));
    const std::array<VkFormat, 1> undeclared{VK_FORMAT_UNDEFINED};
    EXPECT_ERR(g->device.check_storage_image_formats(undeclared, "test"));
}

} // namespace
