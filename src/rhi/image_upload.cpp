// Upload of images whose whole mip chain is supplied by the caller (cooked
// block-compressed textures) and of single-level 3D images (look LUTs).

#include <algorithm>
#include <cstring>
#include <string>
#include <vector>

#include "engine/core/profile.hpp"
#include "engine/rhi/gpu_allocator.hpp"
#include "one_time_commands.hpp"

namespace engine::rhi {

namespace {

[[nodiscard]] std::unexpected<core::Error> fail(std::string what) {
    return std::unexpected(core::Error{std::move(what)});
}

void image_barrier(VkCommandBuffer cmd, VkImage image, std::uint32_t levels,
                   VkPipelineStageFlags2 src_stage, VkAccessFlags2 src_access,
                   VkPipelineStageFlags2 dst_stage, VkAccessFlags2 dst_access,
                   VkImageLayout old_layout, VkImageLayout new_layout) {
    VkImageMemoryBarrier2 b{};
    b.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER_2;
    b.srcStageMask = src_stage;
    b.srcAccessMask = src_access;
    b.dstStageMask = dst_stage;
    b.dstAccessMask = dst_access;
    b.oldLayout = old_layout;
    b.newLayout = new_layout;
    b.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    b.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    b.image = image;
    b.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, levels, 0, 1};
    VkDependencyInfo dep{};
    dep.sType = VK_STRUCTURE_TYPE_DEPENDENCY_INFO;
    dep.imageMemoryBarrierCount = 1;
    dep.pImageMemoryBarriers = &b;
    vkCmdPipelineBarrier2(cmd, &dep);
}

} // namespace

std::expected<GpuImage, core::Error>
upload_device_image_levels(GpuAllocator& allocator, const TransferContext& transfer,
                           const void* data, VkDeviceSize size, VkExtent2D extent,
                           VkFormat format, std::span<const ImageLevelRegion> levels,
                           VkImageUsageFlags usage) {
    QUAT_ZONE_N("upload_image");
    if (levels.empty() || levels.size() > full_mip_levels(extent)) {
        return fail("upload_device_image_levels: bad mip level count");
    }
    for (std::size_t i = 0; i < levels.size(); ++i) {
        const ImageLevelRegion& l = levels[i];
        const auto shift = static_cast<std::uint32_t>(i);
        if (l.width != std::max(extent.width >> shift, 1U) ||
            l.height != std::max(extent.height >> shift, 1U)) {
            return fail("upload_device_image_levels: level " + std::to_string(i) +
                        " extent does not match the mip chain");
        }
        if (l.size == 0 || l.offset > size || l.size > size - l.offset) {
            return fail("upload_device_image_levels: level " + std::to_string(i) +
                        " lies outside the data");
        }
    }

    auto staging = allocator.create_staging_buffer(size);
    if (!staging) return std::unexpected(staging.error());
    std::memcpy(staging->mapped(), data, size);
    if (auto flushed = staging->flush(); !flushed) return std::unexpected(flushed.error());

    const auto level_count = static_cast<std::uint32_t>(levels.size());
    auto image = allocator.create_image(
        format, extent, usage | VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT,
        level_count);
    if (!image) return std::unexpected(image.error());

    std::vector<VkBufferImageCopy> regions(levels.size());
    for (std::size_t i = 0; i < levels.size(); ++i) {
        VkBufferImageCopy& r = regions[i];
        r.bufferOffset = levels[i].offset;
        r.imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, static_cast<std::uint32_t>(i), 0, 1};
        r.imageExtent = {levels[i].width, levels[i].height, 1};
    }

    const VkImage dst = image->handle();
    auto uploaded = detail::run_one_time_commands(transfer, [&](VkCommandBuffer cmd) {
        image_barrier(cmd, dst, level_count, VK_PIPELINE_STAGE_2_TOP_OF_PIPE_BIT,
                      VK_ACCESS_2_NONE, VK_PIPELINE_STAGE_2_ALL_TRANSFER_BIT,
                      VK_ACCESS_2_TRANSFER_WRITE_BIT, VK_IMAGE_LAYOUT_UNDEFINED,
                      VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL);
        vkCmdCopyBufferToImage(cmd, staging->handle(), dst, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
                               static_cast<std::uint32_t>(regions.size()), regions.data());
        image_barrier(cmd, dst, level_count, VK_PIPELINE_STAGE_2_ALL_TRANSFER_BIT,
                      VK_ACCESS_2_TRANSFER_WRITE_BIT, VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT |
                                                          VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT,
                      VK_ACCESS_2_SHADER_SAMPLED_READ_BIT, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
                      VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);
    });
    if (!uploaded) return std::unexpected(uploaded.error());
    return std::move(*image);
}

std::expected<GpuImage, core::Error>
upload_device_image_3d(GpuAllocator& allocator, const TransferContext& transfer,
                       const void* data, VkDeviceSize size, VkExtent3D extent, VkFormat format,
                       VkImageUsageFlags usage) {
    if (extent.width == 0 || extent.height == 0 || extent.depth == 0 || size == 0) {
        return fail("upload_device_image_3d: empty image");
    }
    auto staging = allocator.create_staging_buffer(size);
    if (!staging) return std::unexpected(staging.error());
    std::memcpy(staging->mapped(), data, size);
    if (auto flushed = staging->flush(); !flushed) return std::unexpected(flushed.error());

    auto image = allocator.create_image_3d(
        format, extent, usage | VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT);
    if (!image) return std::unexpected(image.error());

    VkBufferImageCopy region{};
    region.imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
    region.imageExtent = extent;

    const VkImage dst = image->handle();
    auto uploaded = detail::run_one_time_commands(transfer, [&](VkCommandBuffer cmd) {
        image_barrier(cmd, dst, 1, VK_PIPELINE_STAGE_2_TOP_OF_PIPE_BIT, VK_ACCESS_2_NONE,
                      VK_PIPELINE_STAGE_2_ALL_TRANSFER_BIT, VK_ACCESS_2_TRANSFER_WRITE_BIT,
                      VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL);
        vkCmdCopyBufferToImage(cmd, staging->handle(), dst, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
                               1, &region);
        image_barrier(cmd, dst, 1, VK_PIPELINE_STAGE_2_ALL_TRANSFER_BIT,
                      VK_ACCESS_2_TRANSFER_WRITE_BIT, VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT |
                                                          VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT,
                      VK_ACCESS_2_SHADER_SAMPLED_READ_BIT, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
                      VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);
    });
    if (!uploaded) return std::unexpected(uploaded.error());
    return std::move(*image);
}

} // namespace engine::rhi
