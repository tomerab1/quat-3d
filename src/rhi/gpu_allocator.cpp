#include "engine/rhi/gpu_allocator.hpp"

#include <array>
#include <cstring>
#include <format>
#include <utility>

#include "engine/rhi/device.hpp"

namespace engine::rhi {

namespace {

[[nodiscard]] std::unexpected<core::Error> fail(std::string what) {
    return std::unexpected(core::Error{std::move(what)});
}

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
        return fail("vkAllocateCommandBuffers (one-time) failed");
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

    const VkResult r = vkQueueSubmit2(transfer.queue, 1, &submit, VK_NULL_HANDLE);
    if (r == VK_SUCCESS) {
        vkQueueWaitIdle(transfer.queue);
    }
    vkFreeCommandBuffers(transfer.device, transfer.pool, 1, &cmd);
    if (r != VK_SUCCESS) {
        return fail("vkQueueSubmit2 (one-time) failed");
    }
    return {};
}

} // namespace

// ===========================================================================
// GpuBuffer
// ===========================================================================
void GpuBuffer::destroy() noexcept {
    if (buffer_ != VK_NULL_HANDLE && allocator_ != nullptr) {
        vmaDestroyBuffer(allocator_, buffer_, allocation_);
    }
    allocator_ = nullptr;
    buffer_ = VK_NULL_HANDLE;
    allocation_ = nullptr;
    size_ = 0;
    mapped_ = nullptr;
}

GpuBuffer::~GpuBuffer() { destroy(); }

GpuBuffer::GpuBuffer(GpuBuffer&& other) noexcept { *this = std::move(other); }

GpuBuffer& GpuBuffer::operator=(GpuBuffer&& other) noexcept {
    if (this != &other) {
        destroy();
        allocator_  = std::exchange(other.allocator_, nullptr);
        buffer_     = std::exchange(other.buffer_, VK_NULL_HANDLE);
        allocation_ = std::exchange(other.allocation_, nullptr);
        size_       = std::exchange(other.size_, 0);
        mapped_     = std::exchange(other.mapped_, nullptr);
    }
    return *this;
}

// ===========================================================================
// GpuImage
// ===========================================================================
void GpuImage::destroy() noexcept {
    if (image_ != VK_NULL_HANDLE && allocator_ != nullptr) {
        vmaDestroyImage(allocator_, image_, allocation_);
    }
    allocator_ = nullptr;
    image_ = VK_NULL_HANDLE;
    allocation_ = nullptr;
    format_ = VK_FORMAT_UNDEFINED;
    extent_ = VkExtent3D{};
}

GpuImage::~GpuImage() { destroy(); }

GpuImage::GpuImage(GpuImage&& other) noexcept { *this = std::move(other); }

GpuImage& GpuImage::operator=(GpuImage&& other) noexcept {
    if (this != &other) {
        destroy();
        allocator_  = std::exchange(other.allocator_, nullptr);
        image_      = std::exchange(other.image_, VK_NULL_HANDLE);
        allocation_ = std::exchange(other.allocation_, nullptr);
        format_     = std::exchange(other.format_, VK_FORMAT_UNDEFINED);
        extent_     = std::exchange(other.extent_, VkExtent3D{});
    }
    return *this;
}

// ===========================================================================
// GpuAllocator
// ===========================================================================
std::expected<GpuAllocator, core::Error> GpuAllocator::create(const Device& device) {
    VmaVulkanFunctions fns{};
    fns.vkGetInstanceProcAddr = vkGetInstanceProcAddr;
    fns.vkGetDeviceProcAddr = vkGetDeviceProcAddr;

    VmaAllocatorCreateInfo info{};
    info.instance = device.instance();
    info.physicalDevice = device.physical_device();
    info.device = device.handle();
    info.vulkanApiVersion = VK_API_VERSION_1_3;
    info.pVulkanFunctions = &fns;

    GpuAllocator out;
    if (VkResult r = vmaCreateAllocator(&info, &out.allocator_); r != VK_SUCCESS) {
        return fail(std::format("vmaCreateAllocator failed ({})", static_cast<int>(r)));
    }
    return out;
}

GpuAllocator::~GpuAllocator() {
    if (allocator_ != nullptr) {
        vmaDestroyAllocator(allocator_);
        allocator_ = nullptr;
    }
}

GpuAllocator::GpuAllocator(GpuAllocator&& other) noexcept
    : allocator_(std::exchange(other.allocator_, nullptr)) {}

GpuAllocator& GpuAllocator::operator=(GpuAllocator&& other) noexcept {
    if (this != &other) {
        if (allocator_ != nullptr) {
            vmaDestroyAllocator(allocator_);
        }
        allocator_ = std::exchange(other.allocator_, nullptr);
    }
    return *this;
}

std::expected<GpuBuffer, core::Error>
GpuAllocator::create_buffer(VkDeviceSize size, VkBufferUsageFlags usage,
                            VmaMemoryUsage memory_usage, VmaAllocationCreateFlags flags) {
    VkBufferCreateInfo buffer_info{};
    buffer_info.sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO;
    buffer_info.size = size;
    buffer_info.usage = usage;
    buffer_info.sharingMode = VK_SHARING_MODE_EXCLUSIVE;

    VmaAllocationCreateInfo alloc_info{};
    alloc_info.usage = memory_usage;
    alloc_info.flags = flags;

    GpuBuffer buffer;
    VmaAllocationInfo result_info{};
    if (VkResult r = vmaCreateBuffer(allocator_, &buffer_info, &alloc_info,
                                     &buffer.buffer_, &buffer.allocation_, &result_info);
        r != VK_SUCCESS) {
        return fail(std::format("vmaCreateBuffer failed ({})", static_cast<int>(r)));
    }
    buffer.allocator_ = allocator_;
    buffer.size_ = size;
    if (flags & VMA_ALLOCATION_CREATE_MAPPED_BIT) {
        buffer.mapped_ = result_info.pMappedData;
    }
    return buffer;
}

std::expected<GpuBuffer, core::Error>
GpuAllocator::create_staging_buffer(VkDeviceSize size, VkBufferUsageFlags usage) {
    return create_buffer(size, usage, VMA_MEMORY_USAGE_AUTO,
                         VMA_ALLOCATION_CREATE_HOST_ACCESS_SEQUENTIAL_WRITE_BIT
                         | VMA_ALLOCATION_CREATE_MAPPED_BIT);
}

std::expected<GpuImage, core::Error>
GpuAllocator::create_image(VkFormat format, VkExtent2D extent, VkImageUsageFlags usage,
                           std::uint32_t mip_levels) {
    VkImageCreateInfo image_info{};
    image_info.sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO;
    image_info.imageType = VK_IMAGE_TYPE_2D;
    image_info.format = format;
    image_info.extent = {extent.width, extent.height, 1};
    image_info.mipLevels = mip_levels;
    image_info.arrayLayers = 1;
    image_info.samples = VK_SAMPLE_COUNT_1_BIT;
    image_info.tiling = VK_IMAGE_TILING_OPTIMAL;
    image_info.usage = usage;
    image_info.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
    image_info.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;

    VmaAllocationCreateInfo alloc_info{};
    alloc_info.usage = VMA_MEMORY_USAGE_AUTO;

    GpuImage image;
    if (VkResult r = vmaCreateImage(allocator_, &image_info, &alloc_info,
                                    &image.image_, &image.allocation_, nullptr);
        r != VK_SUCCESS) {
        return fail(std::format("vmaCreateImage failed ({})", static_cast<int>(r)));
    }
    image.allocator_ = allocator_;
    image.format_ = format;
    image.extent_ = {extent.width, extent.height, 1};
    return image;
}

// ===========================================================================
// Staging upload + self-test
// ===========================================================================
std::expected<GpuBuffer, core::Error>
upload_device_buffer(GpuAllocator& allocator, const TransferContext& transfer,
                     const void* data, VkDeviceSize size, VkBufferUsageFlags usage) {
    auto staging = allocator.create_staging_buffer(size);
    if (!staging) return std::unexpected(staging.error());
    std::memcpy(staging->mapped(), data, size);

    auto device_buffer = allocator.create_buffer(
        size, usage | VK_BUFFER_USAGE_TRANSFER_DST_BIT, VMA_MEMORY_USAGE_AUTO);
    if (!device_buffer) return std::unexpected(device_buffer.error());

    const VkBuffer src = staging->handle();
    const VkBuffer dst = device_buffer->handle();
    auto copied = run_one_time_commands(transfer, [&](VkCommandBuffer cmd) {
        VkBufferCopy region{0, 0, size};
        vkCmdCopyBuffer(cmd, src, dst, 1, &region);
    });
    if (!copied) return std::unexpected(copied.error());

    return std::move(*device_buffer);
}

std::expected<void, core::Error>
run_gpu_round_trip_self_test(const Device& device, GpuAllocator& allocator) {
    VkCommandPool pool = VK_NULL_HANDLE;
    VkCommandPoolCreateInfo pool_info{};
    pool_info.sType = VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO;
    pool_info.flags = VK_COMMAND_POOL_CREATE_TRANSIENT_BIT;
    pool_info.queueFamilyIndex = device.queue_families().transfer;
    if (vkCreateCommandPool(device.handle(), &pool_info, nullptr, &pool) != VK_SUCCESS) {
        return fail("self-test: vkCreateCommandPool failed");
    }
    const TransferContext transfer{device.handle(), pool, device.transfer_queue()};

    std::array<std::uint8_t, 256> source{};
    for (std::size_t i = 0; i < source.size(); ++i) {
        source[i] = static_cast<std::uint8_t>(i * 7 + 3);
    }
    const VkDeviceSize size = source.size();

    std::expected<void, core::Error> result{};
    auto device_buffer = upload_device_buffer(
        allocator, transfer, source.data(), size, VK_BUFFER_USAGE_TRANSFER_SRC_BIT);
    if (!device_buffer) {
        result = std::unexpected(device_buffer.error());
    } else {
        auto readback = allocator.create_buffer(
            size, VK_BUFFER_USAGE_TRANSFER_DST_BIT, VMA_MEMORY_USAGE_AUTO,
            VMA_ALLOCATION_CREATE_HOST_ACCESS_RANDOM_BIT | VMA_ALLOCATION_CREATE_MAPPED_BIT);
        if (!readback) {
            result = std::unexpected(readback.error());
        } else {
            const VkBuffer src = device_buffer->handle();
            const VkBuffer dst = readback->handle();
            auto copied = run_one_time_commands(transfer, [&](VkCommandBuffer cmd) {
                VkBufferCopy region{0, 0, size};
                vkCmdCopyBuffer(cmd, src, dst, 1, &region);
            });
            if (!copied) {
                result = std::unexpected(copied.error());
            } else if (std::memcmp(readback->mapped(), source.data(), size) != 0) {
                result = fail("self-test: GPU round-trip data mismatch");
            }
        }
    }

    vkDestroyCommandPool(device.handle(), pool, nullptr);
    return result;
}

} // namespace engine::rhi
