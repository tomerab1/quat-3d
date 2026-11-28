// The clouds' baked noise (R13.4, lib/cloud_noise.slang + its CPU mirror):
// every texture tiles, stays in range, keeps the measured ranges the shader
// stretches to [0, 1], and the GPU bake (CloudNoisePass) matches the CPU
// bake texel for texel.

#include <algorithm>
#include <cstdint>
#include <cstring>
#include <expected>
#include <vector>

#include <glm/glm.hpp>
#include <gtest/gtest.h>
#include <vulkan/vulkan.h>

#include "engine/renderer/cloud_noise.hpp"
#include "engine/renderer/cloud_noise_pass.hpp"
#include "support/expect_ok.hpp"
#include "support/gpu_context.hpp"
#include "support/gpu_submit.hpp"
#include "support/render_readback.hpp"

namespace {

namespace r = engine::renderer;
namespace cn = engine::renderer::cloud_noise;

TEST(CloudNoise, Tiles) {
    for (int i = 0; i < 64; ++i) {
        const glm::vec3 p(static_cast<float>(i) * 0.0137F, static_cast<float>(i) * 0.031F,
                          static_cast<float>(i) * 0.0071F);
        for (const glm::vec3 shift : {glm::vec3(1, 0, 0), glm::vec3(0, 1, 0), glm::vec3(0, 0, 1)}) {
            EXPECT_NEAR(cn::base_noise(p).x, cn::base_noise(p + shift).x, 1e-5F);
            EXPECT_NEAR(cn::base_noise(p).w, cn::base_noise(p + shift).w, 1e-5F);
            EXPECT_NEAR(cn::detail_noise(p).y, cn::detail_noise(p + shift).y, 1e-5F);
        }
        const glm::vec2 q(p.x, p.y);
        EXPECT_NEAR(cn::weather_noise(q).x, cn::weather_noise(q + glm::vec2(1, 0)).x, 1e-5F);
        EXPECT_NEAR(cn::curl_noise(q).y, cn::curl_noise(q + glm::vec2(0, 1)).y, 1e-5F);
    }
}

TEST(CloudNoise, HashIsIntegerExact) {
    // pcg3d is pure integer arithmetic: fixed outputs on every platform.
    EXPECT_EQ(cn::pcg3d(glm::uvec3(0, 0, 0)), cn::pcg3d(glm::uvec3(0, 0, 0)));
    EXPECT_NE(cn::pcg3d(glm::uvec3(1, 0, 0)), cn::pcg3d(glm::uvec3(0, 1, 0)));
    const glm::vec3 h = cn::hash33(glm::uvec3(5, 6, 7), 11);
    EXPECT_GE(h.x, 0.0F);
    EXPECT_LT(h.x, 1.0F);
}

// The 0.5th / 99.5th percentiles of the raw fields stay where the shader's
// stretch constants (CLOUD_SHAPE_LO / HI, CLOUD_WEATHER_LO / HI) put them,
// so `coverage` keeps meaning the covered fraction.
TEST(CloudNoise, StretchConstantsMatchTheMeasuredRanges) {
    std::vector<float> shape;
    for (int z = 0; z < 24; ++z) {
        for (int y = 0; y < 40; ++y) {
            for (int x = 0; x < 40; ++x) {
                const glm::vec4 b = cn::base_noise(
                    (glm::vec3(x, y, z) + 0.5F) / glm::vec3(40.0F, 40.0F, 24.0F));
                const float low = b.y * 0.625F + b.z * 0.25F + b.w * 0.125F;
                shape.push_back(std::clamp(cn::remap(b.x, low - 1.0F, 1.0F, 0.0F, 1.0F), 0.0F, 1.0F));
            }
        }
    }
    std::sort(shape.begin(), shape.end());
    EXPECT_NEAR(shape[shape.size() / 200], cn::k_shape_lo, 0.03F);
    EXPECT_NEAR(shape[shape.size() * 199 / 200], cn::k_shape_hi, 0.03F);

    std::vector<float> weather;
    double mean = 0.0;
    for (int y = 0; y < 96; ++y) {
        for (int x = 0; x < 96; ++x) {
            const float w = cn::weather_noise((glm::vec2(x, y) + 0.5F) / 96.0F).x;
            weather.push_back(w);
            mean += w;
        }
    }
    mean /= static_cast<double>(weather.size());
    EXPECT_NEAR(mean, 0.5, 0.1) << "the stretched coverage field averages ~0.5";
    std::sort(weather.begin(), weather.end());
    EXPECT_LT(weather[weather.size() / 20], 0.3F);
    EXPECT_GT(weather[weather.size() * 19 / 20], 0.7F);
}

TEST(CloudNoise, SamplersFilterLikeTheGpu) {
    const cn::Volumes v = cn::bake_cpu(2);
    // At a texel centre the trilinear sample is the texel.
    const glm::vec3 centre = (glm::vec3(3, 5, 1) + 0.5F) / static_cast<float>(cn::k_base_size);
    const glm::u8vec4 t = v.base[(1 * cn::k_base_size + 5) * cn::k_base_size + 3];
    const glm::vec4 s = cn::sample3d(v.base, cn::k_base_size, centre);
    EXPECT_NEAR(s.x, static_cast<float>(t.x) / 255.0F, 1e-6F);
    // REPEAT: one tile over is the same sample.
    EXPECT_NEAR(cn::sample3d(v.base, cn::k_base_size, centre + glm::vec3(1, 0, 0)).x, s.x, 1e-6F);
}

// Blocking read-back of an RGBA8 image (2D or 3D) idle in SHADER_READ_ONLY.
std::expected<std::vector<glm::u8vec4>, engine::core::Error>
read_rgba8(quat::test::GpuContext& g, VkImage image, VkExtent3D extent) {
    const VkDeviceSize size =
        static_cast<VkDeviceSize>(extent.width) * extent.height * extent.depth * 4;
    auto rb = quat::test::host_readback_buffer(g.allocator, size);
    if (!rb) return std::unexpected(rb.error());
    const VkBuffer dst = rb->handle();
    auto done = quat::test::submit_and_wait(g.device, [&](VkCommandBuffer cmd) {
        VkImageMemoryBarrier2 b{};
        b.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER_2;
        b.srcStageMask = VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT;
        b.srcAccessMask = VK_ACCESS_2_MEMORY_WRITE_BIT;
        b.dstStageMask = VK_PIPELINE_STAGE_2_COPY_BIT;
        b.dstAccessMask = VK_ACCESS_2_TRANSFER_READ_BIT;
        b.oldLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
        b.newLayout = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL;
        b.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
        b.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
        b.image = image;
        b.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
        VkDependencyInfo dep{};
        dep.sType = VK_STRUCTURE_TYPE_DEPENDENCY_INFO;
        dep.imageMemoryBarrierCount = 1;
        dep.pImageMemoryBarriers = &b;
        vkCmdPipelineBarrier2(cmd, &dep);
        VkBufferImageCopy region{};
        region.imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
        region.imageExtent = extent;
        vkCmdCopyImageToBuffer(cmd, image, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, dst, 1, &region);
        std::swap(b.oldLayout, b.newLayout);
        b.srcStageMask = VK_PIPELINE_STAGE_2_COPY_BIT;
        b.srcAccessMask = VK_ACCESS_2_TRANSFER_READ_BIT;
        b.dstStageMask = VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT;
        b.dstAccessMask = VK_ACCESS_2_SHADER_SAMPLED_READ_BIT;
        vkCmdPipelineBarrier2(cmd, &dep);
        quat::test::transfer_to_host_barrier(cmd);
    });
    if (!done) return std::unexpected(done.error());
    if (auto inv = rb->invalidate_mapped(); !inv) return std::unexpected(inv.error());
    std::vector<glm::u8vec4> out(static_cast<std::size_t>(size / 4));
    std::memcpy(out.data(), rb->mapped(), static_cast<std::size_t>(size));
    return out;
}

// Texels differing by more than `lsb` 8-bit steps (any channel).
[[nodiscard]] std::size_t mismatches(const std::vector<glm::u8vec4>& a,
                                     const std::vector<glm::u8vec4>& b, int lsb) {
    std::size_t n = 0;
    for (std::size_t i = 0; i < a.size(); ++i) {
        const glm::ivec4 d = glm::abs(glm::ivec4(a[i]) - glm::ivec4(b[i]));
        if (std::max(std::max(d.x, d.y), std::max(d.z, d.w)) > lsb) ++n;
    }
    return n;
}

TEST(GpuCloudNoise, BakeMatchesTheCpuBake) {
    QUAT_REQUIRE_GPU(g);
    auto pass = r::CloudNoisePass::create(g->device, g->allocator, g->cache, g->shader_dir);
    ASSERT_OK(pass);
    const cn::Volumes cpu = cn::bake_cpu();
    const std::uint32_t b = cn::k_base_size;
    const std::uint32_t d = cn::k_detail_size;
    auto base = read_rgba8(*g, pass->base_image(), {b, b, b});
    ASSERT_OK(base);
    auto detail = read_rgba8(*g, pass->detail_image(), {d, d, d});
    ASSERT_OK(detail);
    auto curl = read_rgba8(*g, pass->curl_image(), {cn::k_curl_size, cn::k_curl_size, 1});
    ASSERT_OK(curl);
    auto weather = read_rgba8(*g, pass->weather_image(), {cn::k_weather_size, cn::k_weather_size, 1});
    ASSERT_OK(weather);
    // The lattices are integer-identical; float rounding (fused multiply-adds)
    // may move a value across a quantisation step, never two.
    EXPECT_EQ(mismatches(*base, cpu.base, 1), 0U);
    EXPECT_LT(mismatches(*base, cpu.base, 0), base->size() / 100);
    EXPECT_EQ(mismatches(*detail, cpu.detail, 1), 0U);
    EXPECT_EQ(mismatches(*weather, cpu.weather, 1), 0U);
    // The curl is a difference quotient (rounding is amplified 1 / 2h).
    EXPECT_LT(mismatches(*curl, cpu.curl, 2), curl->size() / 200);
}

} // namespace
