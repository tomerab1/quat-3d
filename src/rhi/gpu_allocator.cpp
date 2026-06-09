#include "engine/rhi/gpu_allocator.hpp"

#include <algorithm>
#include <array>
#include <cmath>
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
// ImageView
// ===========================================================================
void ImageView::destroy() noexcept {
    if (view_ != VK_NULL_HANDLE && device_ != VK_NULL_HANDLE) {
        vkDestroyImageView(device_, view_, nullptr);
    }
    device_ = VK_NULL_HANDLE;
    view_ = VK_NULL_HANDLE;
}

ImageView::~ImageView() { destroy(); }

ImageView::ImageView(ImageView&& other) noexcept { *this = std::move(other); }

ImageView& ImageView::operator=(ImageView&& other) noexcept {
    if (this != &other) {
        destroy();
        device_ = std::exchange(other.device_, VK_NULL_HANDLE);
        view_   = std::exchange(other.view_, VK_NULL_HANDLE);
    }
    return *this;
}

// ===========================================================================
// Sampler
// ===========================================================================
void Sampler::destroy() noexcept {
    if (sampler_ != VK_NULL_HANDLE && device_ != VK_NULL_HANDLE) {
        vkDestroySampler(device_, sampler_, nullptr);
    }
    device_ = VK_NULL_HANDLE;
    sampler_ = VK_NULL_HANDLE;
}

Sampler::~Sampler() { destroy(); }

Sampler::Sampler(Sampler&& other) noexcept { *this = std::move(other); }

Sampler& Sampler::operator=(Sampler&& other) noexcept {
    if (this != &other) {
        destroy();
        device_  = std::exchange(other.device_, VK_NULL_HANDLE);
        sampler_ = std::exchange(other.sampler_, VK_NULL_HANDLE);
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
    // Buffers may request a device address (required by descriptor buffers and
    // bindless), so the allocator must opt into VK_KHR_buffer_device_address.
    info.flags = VMA_ALLOCATOR_CREATE_BUFFER_DEVICE_ADDRESS_BIT;
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
                           std::uint32_t mip_levels, std::uint32_t array_layers,
                           VkImageCreateFlags flags) {
    VkImageCreateInfo image_info{};
    image_info.sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO;
    image_info.flags = flags;
    image_info.imageType = VK_IMAGE_TYPE_2D;
    image_info.format = format;
    image_info.extent = {extent.width, extent.height, 1};
    image_info.mipLevels = mip_levels;
    image_info.arrayLayers = array_layers;
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

VkDeviceAddress buffer_device_address(VkDevice device, const GpuBuffer& buffer) {
    VkBufferDeviceAddressInfo info{};
    info.sType = VK_STRUCTURE_TYPE_BUFFER_DEVICE_ADDRESS_INFO;
    info.buffer = buffer.handle();
    return vkGetBufferDeviceAddress(device, &info);
}

std::uint32_t full_mip_levels(VkExtent2D extent) {
    const std::uint32_t largest = std::max(extent.width, extent.height);
    return largest == 0 ? 1U
                        : static_cast<std::uint32_t>(std::floor(std::log2(largest))) + 1U;
}

std::expected<GpuImage, core::Error>
upload_device_image(GpuAllocator& allocator, const TransferContext& transfer,
                    const void* data, VkDeviceSize size, VkExtent2D extent,
                    VkFormat format, VkImageUsageFlags usage,
                    std::uint32_t mip_levels) {
    mip_levels = std::max(mip_levels, 1U);

    auto staging = allocator.create_staging_buffer(size);
    if (!staging) return std::unexpected(staging.error());
    std::memcpy(staging->mapped(), data, size);

    VkImageUsageFlags image_usage =
        usage | VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT;
    if (mip_levels > 1) image_usage |= VK_IMAGE_USAGE_TRANSFER_SRC_BIT;

    auto image = allocator.create_image(format, extent, image_usage, mip_levels);
    if (!image) return std::unexpected(image.error());

    const VkBuffer src = staging->handle();
    const VkImage  dst = image->handle();
    auto uploaded = run_one_time_commands(transfer, [&](VkCommandBuffer cmd) {
        const auto barrier = [&](std::uint32_t base_mip, std::uint32_t count,
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
            b.image = dst;
            b.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, base_mip, count, 0, 1};
            VkDependencyInfo dep{};
            dep.sType = VK_STRUCTURE_TYPE_DEPENDENCY_INFO;
            dep.imageMemoryBarrierCount = 1;
            dep.pImageMemoryBarriers = &b;
            vkCmdPipelineBarrier2(cmd, &dep);
        };

        // UNDEFINED -> TRANSFER_DST_OPTIMAL (all levels) before the copy.
        barrier(0, mip_levels, VK_PIPELINE_STAGE_2_TOP_OF_PIPE_BIT, VK_ACCESS_2_NONE,
                VK_PIPELINE_STAGE_2_ALL_TRANSFER_BIT, VK_ACCESS_2_TRANSFER_WRITE_BIT,
                VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL);

        VkBufferImageCopy region{};
        region.imageSubresource.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
        region.imageSubresource.layerCount = 1;
        region.imageExtent = {extent.width, extent.height, 1};
        vkCmdCopyBufferToImage(cmd, src, dst, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
                               1, &region);

        // Generate the remaining levels with a linear blit downsample chain:
        // each level becomes TRANSFER_SRC once written, then feeds the next.
        std::int32_t w = static_cast<std::int32_t>(extent.width);
        std::int32_t h = static_cast<std::int32_t>(extent.height);
        for (std::uint32_t mip = 1; mip < mip_levels; ++mip) {
            barrier(mip - 1, 1, VK_PIPELINE_STAGE_2_ALL_TRANSFER_BIT,
                    VK_ACCESS_2_TRANSFER_WRITE_BIT, VK_PIPELINE_STAGE_2_BLIT_BIT,
                    VK_ACCESS_2_TRANSFER_READ_BIT, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
                    VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL);

            const std::int32_t next_w = std::max(w / 2, 1);
            const std::int32_t next_h = std::max(h / 2, 1);
            VkImageBlit blit{};
            blit.srcSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, mip - 1, 0, 1};
            blit.srcOffsets[1] = {w, h, 1};
            blit.dstSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, mip, 0, 1};
            blit.dstOffsets[1] = {next_w, next_h, 1};
            vkCmdBlitImage(cmd, dst, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, dst,
                           VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &blit,
                           VK_FILTER_LINEAR);
            w = next_w;
            h = next_h;
        }

        // Everything -> SHADER_READ_ONLY_OPTIMAL for sampling. Levels 0..n-2 sit
        // in TRANSFER_SRC after feeding the chain; the last is still TRANSFER_DST.
        if (mip_levels > 1) {
            barrier(0, mip_levels - 1, VK_PIPELINE_STAGE_2_BLIT_BIT,
                    VK_ACCESS_2_TRANSFER_READ_BIT, VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT,
                    VK_ACCESS_2_SHADER_SAMPLED_READ_BIT,
                    VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
                    VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);
        }
        barrier(mip_levels - 1, 1, VK_PIPELINE_STAGE_2_ALL_TRANSFER_BIT,
                VK_ACCESS_2_TRANSFER_WRITE_BIT, VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT,
                VK_ACCESS_2_SHADER_SAMPLED_READ_BIT, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
                VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);
    });
    if (!uploaded) return std::unexpected(uploaded.error());

    return std::move(*image);
}

std::expected<ImageView, core::Error>
create_image_view(VkDevice device, VkImage image, VkFormat format,
                  VkImageAspectFlags aspect, std::uint32_t mip_levels,
                  VkImageViewType view_type, std::uint32_t base_mip,
                  std::uint32_t base_layer, std::uint32_t layer_count) {
    VkImageViewCreateInfo info{};
    info.sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO;
    info.image = image;
    info.viewType = view_type;
    info.format = format;
    info.subresourceRange.aspectMask = aspect;
    info.subresourceRange.baseMipLevel = base_mip;
    info.subresourceRange.levelCount = mip_levels;
    info.subresourceRange.baseArrayLayer = base_layer;
    info.subresourceRange.layerCount = layer_count;

    VkImageView view = VK_NULL_HANDLE;
    if (vkCreateImageView(device, &info, nullptr, &view) != VK_SUCCESS) {
        return fail("create_image_view: vkCreateImageView failed");
    }
    return ImageView{device, view};
}

std::expected<Sampler, core::Error>
create_sampler(VkDevice device, SamplerAddress address, std::uint32_t mip_levels,
               float max_anisotropy) {
    VkSamplerAddressMode mode = VK_SAMPLER_ADDRESS_MODE_REPEAT;
    switch (address) {
        case SamplerAddress::repeat:          mode = VK_SAMPLER_ADDRESS_MODE_REPEAT; break;
        case SamplerAddress::clamp_to_edge:   mode = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE; break;
        case SamplerAddress::mirrored_repeat: mode = VK_SAMPLER_ADDRESS_MODE_MIRRORED_REPEAT; break;
    }

    VkSamplerCreateInfo info{};
    info.sType = VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO;
    info.magFilter = VK_FILTER_LINEAR;
    info.minFilter = VK_FILTER_LINEAR;
    info.mipmapMode = VK_SAMPLER_MIPMAP_MODE_LINEAR;
    info.addressModeU = mode;
    info.addressModeV = mode;
    info.addressModeW = mode;
    info.maxLod = static_cast<float>(mip_levels);
    info.anisotropyEnable = max_anisotropy > 0.0F ? VK_TRUE : VK_FALSE;
    info.maxAnisotropy = std::max(max_anisotropy, 1.0F);
    info.borderColor = VK_BORDER_COLOR_FLOAT_OPAQUE_BLACK;

    VkSampler sampler = VK_NULL_HANDLE;
    if (vkCreateSampler(device, &info, nullptr, &sampler) != VK_SUCCESS) {
        return fail("create_sampler: vkCreateSampler failed");
    }
    return Sampler{device, sampler};
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
