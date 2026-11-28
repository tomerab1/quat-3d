#include "engine/rhi/gpu_allocator.hpp"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstring>
#include <format>
#include <utility>
#ifdef ENGINE_DESCRIPTOR_SETS_FALLBACK
#include <cstdio>
#include <iterator>
#include <map>
#include <mutex>
#endif

#include "engine/core/profile.hpp"
#include "engine/rhi/device.hpp"
#include "one_time_commands.hpp"

namespace engine::rhi {

namespace {

[[nodiscard]] std::unexpected<core::Error> fail(std::string what) {
    return std::unexpected(core::Error{std::move(what)});
}

#ifdef ENGINE_DESCRIPTOR_SETS_FALLBACK
// Address -> buffer registry for the classic-sets fallback (see
// find_buffer_for_address in the header). Process-global because GpuBuffer only
// holds the VmaAllocator, not the GpuAllocator that created it, and keyed by
// device: addresses are only unique within one VkDevice, and tests (and tools)
// run several devices in one process. Guarded by a mutex: buffer
// create/destroy happens on asset-loader threads, never per draw, so this
// stays off the hot path required by CLAUDE.md.
struct AddressEntry {
    VkBuffer     buffer = VK_NULL_HANDLE;
    VkDeviceSize size = 0;
};
using AddressKey = std::pair<VkDevice, VkDeviceAddress>;
using BufferKey = std::pair<VkDevice, VkBuffer>;
std::mutex g_address_registry_mutex;
std::map<AddressKey, AddressEntry> g_address_registry; // keyed by (device, base address)
std::map<BufferKey, VkDeviceAddress> g_buffer_to_address; // for unregistering

[[nodiscard]] VkDevice allocator_device(VmaAllocator allocator) {
    VmaAllocatorInfo allocator_info{};
    vmaGetAllocatorInfo(allocator, &allocator_info);
    return allocator_info.device;
}

void register_buffer_address(VmaAllocator allocator, VkBuffer buffer, VkDeviceSize size) {
    const VkDevice device = allocator_device(allocator);
    VkBufferDeviceAddressInfo info{};
    info.sType = VK_STRUCTURE_TYPE_BUFFER_DEVICE_ADDRESS_INFO;
    info.buffer = buffer;
    const VkDeviceAddress address = vkGetBufferDeviceAddress(device, &info);

    const std::lock_guard<std::mutex> lock(g_address_registry_mutex);
    // Live buffers of one device never share addresses; an overlap means
    // the driver aliases them and descriptor writes would pick either one.
    auto next = g_address_registry.lower_bound({device, address});
    const bool overlaps_next = next != g_address_registry.end() && next->first.first == device &&
                               next->first.second < address + size;
    bool overlaps_prev = false;
    if (next != g_address_registry.begin()) {
        const auto prev = std::prev(next);
        overlaps_prev = prev->first.first == device &&
                        prev->first.second + prev->second.size > address;
    }
    if (overlaps_next || overlaps_prev || address == 0) {
        std::fprintf(stderr,
                     "[rhi] classic descriptor fallback: buffer address 0x%llx (+%llu) overlaps a "
                     "live buffer; descriptor writes into it are ambiguous\n",
                     static_cast<unsigned long long>(address),
                     static_cast<unsigned long long>(size));
    }
    g_address_registry[{device, address}] = AddressEntry{buffer, size};
    g_buffer_to_address[{device, buffer}] = address;
}

void unregister_buffer_address(VmaAllocator allocator, VkBuffer buffer) {
    const VkDevice device = allocator_device(allocator);
    const std::lock_guard<std::mutex> lock(g_address_registry_mutex);
    const auto it = g_buffer_to_address.find({device, buffer});
    if (it == g_buffer_to_address.end()) return;
    // Erase the address entry only while it still names this buffer: a
    // later registration at the same address owns it now.
    if (const auto entry = g_address_registry.find({device, it->second});
        entry != g_address_registry.end() && entry->second.buffer == buffer) {
        g_address_registry.erase(entry);
    }
    g_buffer_to_address.erase(it);
}
#endif

} // namespace

// ===========================================================================
// GpuBuffer
// ===========================================================================
void GpuBuffer::destroy() noexcept {
    if (buffer_ != VK_NULL_HANDLE && allocator_ != nullptr) {
#ifdef ENGINE_DESCRIPTOR_SETS_FALLBACK
        unregister_buffer_address(allocator_, buffer_);
#endif
        vmaDestroyBuffer(allocator_, buffer_, allocation_);
    }
    allocator_ = nullptr;
    buffer_ = VK_NULL_HANDLE;
    allocation_ = nullptr;
    size_ = 0;
    mapped_ = nullptr;
}

std::expected<void, core::Error> GpuBuffer::invalidate_mapped() const {
    if (buffer_ == VK_NULL_HANDLE || allocator_ == nullptr) return fail("invalidate: no buffer");
    if (const VkResult r = vmaInvalidateAllocation(allocator_, allocation_, 0, VK_WHOLE_SIZE);
        r != VK_SUCCESS) {
        return fail(std::format("vmaInvalidateAllocation failed ({})", static_cast<int>(r)));
    }
    return {};
}

GpuBuffer::~GpuBuffer() { destroy(); }

std::expected<void, core::Error> GpuBuffer::flush(VkDeviceSize offset, VkDeviceSize size) const {
    if (allocation_ == nullptr) return {};
    if (VkResult r = vmaFlushAllocation(allocator_, allocation_, offset, size); r != VK_SUCCESS) {
        return fail(std::format("vmaFlushAllocation failed ({})", static_cast<int>(r)));
    }
    return {};
}

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
    // The driver's own budget / usage per heap when the device enabled
    // VK_EXT_memory_budget (profiling, R4); VMA estimates otherwise.
    if (device.memory_budget()) info.flags |= VMA_ALLOCATOR_CREATE_EXT_MEMORY_BUDGET_BIT;
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

void GpuAllocator::heap_stats(std::vector<GpuHeapStats>& out) const {
    out.clear();
    if (allocator_ == nullptr) return;
    const VkPhysicalDeviceMemoryProperties* props = nullptr;
    vmaGetMemoryProperties(allocator_, &props);
    if (props == nullptr) return;
    std::array<VmaBudget, VK_MAX_MEMORY_HEAPS> budgets{};
    vmaGetHeapBudgets(allocator_, budgets.data());
    out.reserve(props->memoryHeapCount);
    for (std::uint32_t h = 0; h < props->memoryHeapCount; ++h) {
        const VmaBudget& b = budgets[h];
        out.push_back(GpuHeapStats{
            .heap = h,
            .device_local = (props->memoryHeaps[h].flags & VK_MEMORY_HEAP_DEVICE_LOCAL_BIT) != 0,
            .size = props->memoryHeaps[h].size,
            .budget = b.budget,
            .usage = b.usage,
            .block_bytes = b.statistics.blockBytes,
            .allocation_bytes = b.statistics.allocationBytes,
            .allocations = b.statistics.allocationCount});
    }
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
#ifdef ENGINE_DESCRIPTOR_SETS_FALLBACK
    if (usage & VK_BUFFER_USAGE_SHADER_DEVICE_ADDRESS_BIT) {
        register_buffer_address(allocator_, buffer.buffer_, size);
    }
#endif
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
                           VkImageCreateFlags flags, std::span<const VkFormat> view_formats) {
    VkImageCreateInfo image_info{};
    image_info.sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO;
    image_info.flags = flags;
    VkImageFormatListCreateInfo format_list{};
    if (!view_formats.empty()) {
        format_list.sType = VK_STRUCTURE_TYPE_IMAGE_FORMAT_LIST_CREATE_INFO;
        format_list.viewFormatCount = static_cast<std::uint32_t>(view_formats.size());
        format_list.pViewFormats = view_formats.data();
        image_info.pNext = &format_list;
    }
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

std::expected<GpuImage, core::Error>
GpuAllocator::create_image_3d(VkFormat format, VkExtent3D extent, VkImageUsageFlags usage) {
    VkImageCreateInfo image_info{};
    image_info.sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO;
    image_info.imageType = VK_IMAGE_TYPE_3D;
    image_info.format = format;
    image_info.extent = extent;
    image_info.mipLevels = 1;
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
        return fail(std::format("vmaCreateImage (3D) failed ({})", static_cast<int>(r)));
    }
    image.allocator_ = allocator_;
    image.format_ = format;
    image.extent_ = extent;
    return image;
}

// ===========================================================================
// Staging upload
// ===========================================================================
std::expected<GpuBuffer, core::Error>
upload_device_buffer(GpuAllocator& allocator, const TransferContext& transfer,
                     const void* data, VkDeviceSize size, VkBufferUsageFlags usage) {
    QUAT_ZONE_N("upload_buffer");
    auto staging = allocator.create_staging_buffer(size);
    if (!staging) return std::unexpected(staging.error());
    std::memcpy(staging->mapped(), data, size);
    if (auto flushed = staging->flush(); !flushed) return std::unexpected(flushed.error());

    auto device_buffer = allocator.create_buffer(
        size, usage | VK_BUFFER_USAGE_TRANSFER_DST_BIT, VMA_MEMORY_USAGE_AUTO);
    if (!device_buffer) return std::unexpected(device_buffer.error());

    const VkBuffer src = staging->handle();
    const VkBuffer dst = device_buffer->handle();
    auto copied = detail::run_one_time_commands(transfer, [&](VkCommandBuffer cmd) {
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

#ifdef ENGINE_DESCRIPTOR_SETS_FALLBACK
std::optional<BufferAddressRange> find_buffer_for_address(VkDevice device,
                                                          VkDeviceAddress address) {
    const std::lock_guard<std::mutex> lock(g_address_registry_mutex);
    auto it = g_address_registry.upper_bound({device, address});
    if (it == g_address_registry.begin()) return std::nullopt;
    --it; // greatest (device, base address) <= (device, address)
    if (it->first.first != device || address >= it->first.second + it->second.size) {
        return std::nullopt;
    }
    return BufferAddressRange{it->second.buffer, address - it->first.second};
}
#endif

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
    QUAT_ZONE_N("upload_image");
    mip_levels = std::max(mip_levels, 1U);

    auto staging = allocator.create_staging_buffer(size);
    if (!staging) return std::unexpected(staging.error());
    std::memcpy(staging->mapped(), data, size);
    if (auto flushed = staging->flush(); !flushed) return std::unexpected(flushed.error());

    VkImageUsageFlags image_usage =
        usage | VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT;
    if (mip_levels > 1) image_usage |= VK_IMAGE_USAGE_TRANSFER_SRC_BIT;

    auto image = allocator.create_image(format, extent, image_usage, mip_levels);
    if (!image) return std::unexpected(image.error());

    const VkBuffer src = staging->handle();
    const VkImage  dst = image->handle();
    auto uploaded = detail::run_one_time_commands(transfer, [&](VkCommandBuffer cmd) {
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
                  std::uint32_t base_layer, std::uint32_t layer_count,
                  VkComponentMapping components) {
    VkImageViewCreateInfo info{};
    info.sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO;
    info.image = image;
    info.viewType = view_type;
    info.format = format;
    info.components = components;
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
               float max_anisotropy, float mip_lod_bias) {
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
    // maxSamplerLodBias is at least 2 on every conformant device.
    info.mipLodBias = std::clamp(mip_lod_bias, -2.0F, 2.0F);
    info.anisotropyEnable = max_anisotropy > 0.0F ? VK_TRUE : VK_FALSE;
    info.maxAnisotropy = std::max(max_anisotropy, 1.0F);
    info.borderColor = VK_BORDER_COLOR_FLOAT_OPAQUE_BLACK;

    VkSampler sampler = VK_NULL_HANDLE;
    if (vkCreateSampler(device, &info, nullptr, &sampler) != VK_SUCCESS) {
        return fail("create_sampler: vkCreateSampler failed");
    }
    return Sampler{device, sampler};
}

std::expected<Sampler, core::Error> create_sampler(VkDevice device, const SamplerDesc& desc) {
    VkSamplerCreateInfo info{};
    info.sType = VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO;
    info.magFilter = desc.filter;
    info.minFilter = desc.filter;
    info.mipmapMode = desc.mip_mode;
    info.addressModeU = desc.address;
    info.addressModeV = desc.address;
    info.addressModeW = desc.address;
    info.maxLod = desc.max_lod;
    info.compareEnable = desc.compare ? VK_TRUE : VK_FALSE;
    info.compareOp = desc.compare_op;
    info.borderColor = desc.border;

    VkSampler sampler = VK_NULL_HANDLE;
    if (vkCreateSampler(device, &info, nullptr, &sampler) != VK_SUCCESS) {
        return fail("create_sampler: vkCreateSampler failed");
    }
    return Sampler{device, sampler};
}

} // namespace engine::rhi
