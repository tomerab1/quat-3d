#pragma once

// Private to src/rhi: blocking one-time command submission shared by the
// staging upload helpers (gpu_allocator.cpp, image_upload.cpp).

#include <expected>

#include <vulkan/vulkan.h>

#include "engine/core/error.hpp"
#include "engine/rhi/gpu_allocator.hpp"
#include "engine/rhi/queue_submit.hpp"

namespace engine::rhi::detail {

// Records `record` into a one-time command buffer from the transfer pool,
// submits it via synchronization2, and blocks until the queue is idle.
template <typename Record>
[[nodiscard]] std::expected<void, core::Error>
run_one_time_commands(const TransferContext& transfer, Record&& record) {
    VkCommandBufferAllocateInfo alloc{};
    alloc.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO;
    alloc.commandPool = transfer.pool;
    alloc.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
    alloc.commandBufferCount = 1;

    VkCommandBuffer cmd = VK_NULL_HANDLE;
    if (vkAllocateCommandBuffers(transfer.device, &alloc, &cmd) != VK_SUCCESS) {
        return std::unexpected(core::Error{"vkAllocateCommandBuffers (one-time) failed"});
    }

    VkCommandBufferBeginInfo begin{};
    begin.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO;
    begin.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
    vkBeginCommandBuffer(cmd, &begin);
    record(cmd);
    vkEndCommandBuffer(cmd);

    VkCommandBufferSubmitInfo cmd_info{};
    cmd_info.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_SUBMIT_INFO;
    cmd_info.commandBuffer = cmd;

    VkSubmitInfo2 submit{};
    submit.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO_2;
    submit.commandBufferInfoCount = 1;
    submit.pCommandBufferInfos = &cmd_info;

    auto done = submit_and_wait_idle(transfer.queue, submit, "one-time commands");
    vkFreeCommandBuffers(transfer.device, transfer.pool, 1, &cmd);
    return done;
}

} // namespace engine::rhi::detail
