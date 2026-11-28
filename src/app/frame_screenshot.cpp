// FrameRenderer's QUAT_SCREENSHOT capture: the copy recorded into the final
// frame and the PPM written once it finished (split from frame_renderer.cpp).

#include <cstdint>
#include <cstdio>
#include <fstream>
#include <string>
#include <vector>

#include "app/frame_renderer.hpp"
#include "engine/core/utf8_path.hpp"
#include "engine/rhi/gpu_allocator.hpp"
#include "engine/rhi/swapchain.hpp"

namespace quat::app {

bool FrameRenderer::record_screenshot_copy(VkCommandBuffer cmd) {
    // Screenshot capture on the final frame: the graph leaves the swapchain
    // image in PRESENT_SRC — round-trip it through TRANSFER_SRC and copy to
    // a host-visible buffer, written out as PPM after the queue idles.
    const VkExtent2D present_extent = swapchain_->extent();
    const VkDeviceSize shot_bytes =
        static_cast<VkDeviceSize>(present_extent.width) * present_extent.height * 4;
    if (screenshot_buffer_.handle() == VK_NULL_HANDLE) {
        auto buf = allocator_->create_buffer(shot_bytes, VK_BUFFER_USAGE_TRANSFER_DST_BIT,
                                             VMA_MEMORY_USAGE_AUTO,
                                             VMA_ALLOCATION_CREATE_HOST_ACCESS_RANDOM_BIT |
                                                 VMA_ALLOCATION_CREATE_MAPPED_BIT);
        if (buf) screenshot_buffer_ = std::move(*buf);
    }
    if (screenshot_buffer_.handle() == VK_NULL_HANDLE) return false;

    const VkImage shot_image = swapchain_->images()[image_index_];
    VkImageMemoryBarrier2 to_src{};
    to_src.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER_2;
    to_src.srcStageMask = VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT;
    to_src.srcAccessMask = VK_ACCESS_2_MEMORY_WRITE_BIT;
    to_src.dstStageMask = VK_PIPELINE_STAGE_2_COPY_BIT;
    to_src.dstAccessMask = VK_ACCESS_2_TRANSFER_READ_BIT;
    to_src.oldLayout = VK_IMAGE_LAYOUT_PRESENT_SRC_KHR;
    to_src.newLayout = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL;
    to_src.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    to_src.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    to_src.image = shot_image;
    to_src.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
    VkDependencyInfo dep{};
    dep.sType = VK_STRUCTURE_TYPE_DEPENDENCY_INFO;
    dep.imageMemoryBarrierCount = 1;
    dep.pImageMemoryBarriers = &to_src;
    vkCmdPipelineBarrier2(cmd, &dep);

    VkBufferImageCopy region{};
    region.imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
    region.imageExtent = {present_extent.width, present_extent.height, 1};
    vkCmdCopyImageToBuffer(cmd, shot_image, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
                           screenshot_buffer_.handle(), 1, &region);

    VkImageMemoryBarrier2 to_present = to_src;
    to_present.srcStageMask = VK_PIPELINE_STAGE_2_COPY_BIT;
    to_present.srcAccessMask = VK_ACCESS_2_TRANSFER_READ_BIT;
    // Chained onto by the render-finished signal (see k_present_signal_stage).
    to_present.dstStageMask = k_present_signal_stage;
    to_present.dstAccessMask = 0;
    to_present.oldLayout = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL;
    to_present.newLayout = VK_IMAGE_LAYOUT_PRESENT_SRC_KHR;
    dep.pImageMemoryBarriers = &to_present;
    vkCmdPipelineBarrier2(cmd, &dep);
    return true;
}

void FrameRenderer::write_screenshot() const {
    const VkExtent2D present_extent = swapchain_->extent();
    // fs::path all the way: a non-ASCII path (e.g. CJK on Windows) must not go
    // through a narrow fopen.
    const std::string shown = engine::core::path_to_utf8(*screenshot_path_);
    const bool bgra = swapchain_->format() == VK_FORMAT_B8G8R8A8_SRGB ||
                      swapchain_->format() == VK_FORMAT_B8G8R8A8_UNORM;
    if (std::ofstream out(*screenshot_path_, std::ios::binary | std::ios::trunc); out) {
        out << "P6\n" << present_extent.width << ' ' << present_extent.height << "\n255\n";
        const auto* px = static_cast<const std::uint8_t*>(screenshot_buffer_.mapped());
        std::vector<std::uint8_t> row(static_cast<std::size_t>(present_extent.width) * 3);
        for (std::uint32_t y = 0; y < present_extent.height; ++y) {
            for (std::uint32_t x = 0; x < present_extent.width; ++x) {
                const std::uint8_t* p =
                    px + (static_cast<std::size_t>(y) * present_extent.width + x) * 4;
                row[x * 3 + 0] = bgra ? p[2] : p[0];
                row[x * 3 + 1] = p[1];
                row[x * 3 + 2] = bgra ? p[0] : p[2];
            }
            out.write(reinterpret_cast<const char*>(row.data()),
                      static_cast<std::streamsize>(row.size()));
        }
        out.close();
        if (out) {
            std::fprintf(stderr, "[screenshot] wrote %s\n", shown.c_str());
        } else {
            std::fprintf(stderr, "[screenshot] write failed for %s\n", shown.c_str());
        }
    } else {
        std::fprintf(stderr, "[screenshot] could not open %s\n", shown.c_str());
    }
}

} // namespace quat::app
