// GPU allocator: staged upload to a device-local buffer, read back through
// the dedicated transfer queue.

#include <array>
#include <cstdint>
#include <cstring>

#include <gtest/gtest.h>
#include <vulkan/vulkan.h>

#include "engine/rhi/device.hpp"
#include "engine/rhi/gpu_allocator.hpp"
#include "support/expect_ok.hpp"
#include "support/gpu_context.hpp"

namespace {

using engine::rhi::Device;
using engine::rhi::TransferContext;

// A transient command pool on the device's transfer family, with a blocking
// one-shot submit on the transfer queue.
class TransferPool {
public:
    explicit TransferPool(const Device& device) : device_(device) {
        VkCommandPoolCreateInfo info{};
        info.sType = VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO;
        info.flags = VK_COMMAND_POOL_CREATE_TRANSIENT_BIT;
        info.queueFamilyIndex = device.queue_families().transfer;
        if (vkCreateCommandPool(device.handle(), &info, nullptr, &pool_) != VK_SUCCESS) {
            pool_ = VK_NULL_HANDLE;
        }
    }
    ~TransferPool() {
        if (pool_ != VK_NULL_HANDLE) vkDestroyCommandPool(device_.handle(), pool_, nullptr);
    }
    TransferPool(const TransferPool&) = delete;
    TransferPool& operator=(const TransferPool&) = delete;

    [[nodiscard]] bool valid() const { return pool_ != VK_NULL_HANDLE; }
    [[nodiscard]] TransferContext context() const {
        return {device_.handle(), pool_, device_.transfer_queue()};
    }

    // Records `record` into a one-time command buffer and waits for it.
    template <typename Record>
    [[nodiscard]] bool submit_and_wait(Record&& record) const {
        VkCommandBufferAllocateInfo alloc{};
        alloc.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO;
        alloc.commandPool = pool_;
        alloc.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
        alloc.commandBufferCount = 1;
        VkCommandBuffer cmd = VK_NULL_HANDLE;
        if (vkAllocateCommandBuffers(device_.handle(), &alloc, &cmd) != VK_SUCCESS) return false;

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
        const VkQueue queue = device_.transfer_queue();
        const bool ok = vkQueueSubmit2(queue, 1, &submit, VK_NULL_HANDLE) == VK_SUCCESS &&
                        vkQueueWaitIdle(queue) == VK_SUCCESS;
        vkFreeCommandBuffers(device_.handle(), pool_, 1, &cmd);
        return ok;
    }

private:
    const Device& device_;
    VkCommandPool pool_ = VK_NULL_HANDLE;
};

TEST(GpuAllocator, StagedUploadRoundTripsThroughTheTransferQueue) {
    QUAT_REQUIRE_GPU(g);
    const TransferPool transfer(g->device);
    ASSERT_TRUE(transfer.valid()) << "vkCreateCommandPool on the transfer family failed";

    std::array<std::uint8_t, 256> source{};
    for (std::size_t i = 0; i < source.size(); ++i) {
        source[i] = static_cast<std::uint8_t>(i * 7 + 3);
    }
    const VkDeviceSize size = source.size();

    auto device_buffer = engine::rhi::upload_device_buffer(
        g->allocator, transfer.context(), source.data(), size, VK_BUFFER_USAGE_TRANSFER_SRC_BIT);
    ASSERT_OK(device_buffer);
    EXPECT_EQ(device_buffer->size(), size);

    auto readback = g->allocator.create_buffer(
        size, VK_BUFFER_USAGE_TRANSFER_DST_BIT, VMA_MEMORY_USAGE_AUTO,
        VMA_ALLOCATION_CREATE_HOST_ACCESS_RANDOM_BIT | VMA_ALLOCATION_CREATE_MAPPED_BIT);
    ASSERT_OK(readback);
    ASSERT_NE(readback->mapped(), nullptr);

    const VkBuffer src = device_buffer->handle();
    const VkBuffer dst = readback->handle();
    ASSERT_TRUE(transfer.submit_and_wait([&](VkCommandBuffer cmd) {
        const VkBufferCopy region{0, 0, size};
        vkCmdCopyBuffer(cmd, src, dst, 1, &region);
    }));

    const auto* got = static_cast<const std::uint8_t*>(readback->mapped());
    for (std::size_t i = 0; i < source.size(); ++i) {
        ASSERT_EQ(got[i], source[i]) << "first mismatch at byte " << i;
    }
}

} // namespace
