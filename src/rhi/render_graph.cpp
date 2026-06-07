#include "engine/rhi/render_graph.hpp"

#include <algorithm>
#include <array>
#include <cstring>
#include <format>
#include <span>
#include <unordered_set>
#include <utility>

#include "engine/rhi/device.hpp"

namespace engine::rhi {

namespace {

[[nodiscard]] std::unexpected<core::Error> fail(std::string what) {
    return std::unexpected(core::Error{std::move(what)});
}

[[nodiscard]] bool is_depth_format(VkFormat format) {
    switch (format) {
    case VK_FORMAT_D16_UNORM:
    case VK_FORMAT_D32_SFLOAT:
    case VK_FORMAT_D16_UNORM_S8_UINT:
    case VK_FORMAT_D24_UNORM_S8_UINT:
    case VK_FORMAT_D32_SFLOAT_S8_UINT:
    case VK_FORMAT_X8_D24_UNORM_PACK32:
        return true;
    default:
        return false;
    }
}

[[nodiscard]] bool has_stencil(VkFormat format) {
    switch (format) {
    case VK_FORMAT_D16_UNORM_S8_UINT:
    case VK_FORMAT_D24_UNORM_S8_UINT:
    case VK_FORMAT_D32_SFLOAT_S8_UINT:
        return true;
    default:
        return false;
    }
}

[[nodiscard]] VkImageAspectFlags aspect_for_format(VkFormat format) {
    if (!is_depth_format(format)) {
        return VK_IMAGE_ASPECT_COLOR_BIT;
    }
    VkImageAspectFlags aspect = VK_IMAGE_ASPECT_DEPTH_BIT;
    if (has_stencil(format)) {
        aspect |= VK_IMAGE_ASPECT_STENCIL_BIT;
    }
    return aspect;
}

// The layout + synchronization scope a given usage demands. Transfer usages map
// to ALL_TRANSFER (covers both clears and copies); shader reads default to the
// fragment stage (compute usages use their own enums).
struct UsageState {
    VkImageLayout         layout;
    VkPipelineStageFlags2 stage;
    VkAccessFlags2        access;
};

[[nodiscard]] UsageState usage_state(ResourceUsage usage) {
    switch (usage) {
    case ResourceUsage::color_attachment:
        return {VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL,
                VK_PIPELINE_STAGE_2_COLOR_ATTACHMENT_OUTPUT_BIT,
                VK_ACCESS_2_COLOR_ATTACHMENT_WRITE_BIT | VK_ACCESS_2_COLOR_ATTACHMENT_READ_BIT};
    case ResourceUsage::depth_attachment:
        return {VK_IMAGE_LAYOUT_DEPTH_ATTACHMENT_OPTIMAL,
                VK_PIPELINE_STAGE_2_EARLY_FRAGMENT_TESTS_BIT
                    | VK_PIPELINE_STAGE_2_LATE_FRAGMENT_TESTS_BIT,
                VK_ACCESS_2_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT
                    | VK_ACCESS_2_DEPTH_STENCIL_ATTACHMENT_READ_BIT};
    case ResourceUsage::sampled:
        return {VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
                VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT,
                VK_ACCESS_2_SHADER_SAMPLED_READ_BIT};
    case ResourceUsage::storage_read:
        return {VK_IMAGE_LAYOUT_GENERAL,
                VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT,
                VK_ACCESS_2_SHADER_STORAGE_READ_BIT};
    case ResourceUsage::storage_write:
        return {VK_IMAGE_LAYOUT_GENERAL,
                VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT,
                VK_ACCESS_2_SHADER_STORAGE_WRITE_BIT};
    case ResourceUsage::transfer_src:
        return {VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
                VK_PIPELINE_STAGE_2_ALL_TRANSFER_BIT, VK_ACCESS_2_TRANSFER_READ_BIT};
    case ResourceUsage::transfer_dst:
        return {VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
                VK_PIPELINE_STAGE_2_ALL_TRANSFER_BIT, VK_ACCESS_2_TRANSFER_WRITE_BIT};
    case ResourceUsage::present:
        return {VK_IMAGE_LAYOUT_PRESENT_SRC_KHR, VK_PIPELINE_STAGE_2_NONE, 0};
    }
    return {VK_IMAGE_LAYOUT_UNDEFINED, VK_PIPELINE_STAGE_2_NONE, 0};
}

[[nodiscard]] VkImageMemoryBarrier2
make_barrier(VkImage image, VkImageAspectFlags aspect,
             VkImageLayout old_layout, VkImageLayout new_layout,
             VkPipelineStageFlags2 src_stage, VkAccessFlags2 src_access,
             VkPipelineStageFlags2 dst_stage, VkAccessFlags2 dst_access) {
    VkImageMemoryBarrier2 barrier{};
    barrier.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER_2;
    barrier.srcStageMask = src_stage;
    barrier.srcAccessMask = src_access;
    barrier.dstStageMask = dst_stage;
    barrier.dstAccessMask = dst_access;
    barrier.oldLayout = old_layout;
    barrier.newLayout = new_layout;
    barrier.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    barrier.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    barrier.image = image;
    barrier.subresourceRange = {aspect, 0, 1, 0, 1};
    return barrier;
}

void submit_barriers(VkCommandBuffer cmd, std::span<const VkImageMemoryBarrier2> barriers) {
    if (barriers.empty()) return;
    VkDependencyInfo dep{};
    dep.sType = VK_STRUCTURE_TYPE_DEPENDENCY_INFO;
    dep.imageMemoryBarrierCount = static_cast<std::uint32_t>(barriers.size());
    dep.pImageMemoryBarriers = barriers.data();
    vkCmdPipelineBarrier2(cmd, &dep);
}

} // namespace

// ===========================================================================
// TransientImagePool
// ===========================================================================
TransientImagePool::TransientImagePool(const Device& device, GpuAllocator& allocator)
    : device_(&device), allocator_(&allocator) {}

void TransientImagePool::destroy() noexcept {
    if (device_ != nullptr) {
        for (Entry& e : entries_) {
            if (e.view != VK_NULL_HANDLE) {
                vkDestroyImageView(device_->handle(), e.view, nullptr);
            }
        }
    }
    entries_.clear();   // GpuImage destructors release the VMA allocations
    device_ = nullptr;
    allocator_ = nullptr;
}

TransientImagePool::~TransientImagePool() { destroy(); }

TransientImagePool::TransientImagePool(TransientImagePool&& other) noexcept
    : device_(std::exchange(other.device_, nullptr)),
      allocator_(std::exchange(other.allocator_, nullptr)),
      entries_(std::move(other.entries_)) {}

TransientImagePool& TransientImagePool::operator=(TransientImagePool&& other) noexcept {
    if (this != &other) {
        destroy();
        device_ = std::exchange(other.device_, nullptr);
        allocator_ = std::exchange(other.allocator_, nullptr);
        entries_ = std::move(other.entries_);
    }
    return *this;
}

std::expected<ResourceBinding, core::Error>
TransientImagePool::acquire(VkFormat format, VkExtent2D extent, VkImageUsageFlags usage) {
    if (allocator_ == nullptr || device_ == nullptr) {
        return fail("TransientImagePool::acquire on a null pool");
    }

    for (Entry& e : entries_) {
        if (!e.in_use && e.format == format && e.usage == usage
            && e.extent.width == extent.width && e.extent.height == extent.height) {
            e.in_use = true;
            return ResourceBinding{e.image.handle(), e.view, e.format, e.extent};
        }
    }

    auto image = allocator_->create_image(format, extent, usage);
    if (!image) return std::unexpected(image.error());

    // A view is only valid (and only needed) when the image can be bound as an
    // attachment, sampled, or storage image. Pure transfer scratch images skip it.
    constexpr VkImageUsageFlags view_capable =
        VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_STORAGE_BIT
        | VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT
        | VK_IMAGE_USAGE_INPUT_ATTACHMENT_BIT | VK_IMAGE_USAGE_TRANSIENT_ATTACHMENT_BIT;

    VkImageView view = VK_NULL_HANDLE;
    if (usage & view_capable) {
        VkImageViewCreateInfo view_info{};
        view_info.sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO;
        view_info.image = image->handle();
        view_info.viewType = VK_IMAGE_VIEW_TYPE_2D;
        view_info.format = format;
        view_info.subresourceRange = {aspect_for_format(format), 0, 1, 0, 1};
        if (vkCreateImageView(device_->handle(), &view_info, nullptr, &view) != VK_SUCCESS) {
            return fail("TransientImagePool: vkCreateImageView failed");
        }
    }

    Entry entry;
    entry.image = std::move(*image);
    entry.view = view;
    entry.format = format;
    entry.extent = extent;
    entry.usage = usage;
    entry.in_use = true;
    entries_.push_back(std::move(entry));

    const Entry& stored = entries_.back();
    return ResourceBinding{stored.image.handle(), stored.view, stored.format, stored.extent};
}

void TransientImagePool::reset() {
    for (Entry& e : entries_) {
        e.in_use = false;
    }
}

// ===========================================================================
// PassContext
// ===========================================================================
ResourceBinding PassContext::resolve(ResourceHandle handle) const {
    return graph_->binding(handle);
}

// ===========================================================================
// RenderGraph — declaration
// ===========================================================================
ResourceHandle RenderGraph::import_image(const char* name, VkImage image, VkImageView view,
                                         VkFormat format, VkExtent2D extent,
                                         VkImageLayout current_layout,
                                         VkImageLayout final_layout) {
    Resource r;
    r.name = name;
    r.imported = true;
    r.image = image;
    r.view = view;
    r.format = format;
    r.extent = extent;
    r.initial_layout = current_layout;
    r.final_layout = final_layout;
    resources_.push_back(std::move(r));
    return ResourceHandle{static_cast<std::uint32_t>(resources_.size() - 1)};
}

ResourceHandle RenderGraph::create_transient_image(const char* name, VkFormat format,
                                                   VkExtent2D extent, VkImageUsageFlags usage) {
    Resource r;
    r.name = name;
    r.imported = false;
    r.format = format;
    r.extent = extent;
    r.usage = usage;
    resources_.push_back(std::move(r));
    return ResourceHandle{static_cast<std::uint32_t>(resources_.size() - 1)};
}

RenderGraph::PassBuilder RenderGraph::add_pass(const char* name, PassType type) {
    Pass p;
    p.name = name;
    p.type = type;
    passes_.push_back(std::move(p));
    return PassBuilder{*this, static_cast<std::uint32_t>(passes_.size() - 1)};
}

RenderGraph::PassBuilder&
RenderGraph::PassBuilder::reads(ResourceHandle handle, ResourceUsage usage) {
    graph_->passes_[pass_].accesses.push_back({handle.index, usage, /*is_write=*/false});
    return *this;
}

RenderGraph::PassBuilder&
RenderGraph::PassBuilder::writes(ResourceHandle handle, ResourceUsage usage) {
    graph_->passes_[pass_].accesses.push_back({handle.index, usage, /*is_write=*/true});
    return *this;
}

RenderGraph::PassBuilder& RenderGraph::PassBuilder::execute(PassExecuteFn fn) {
    graph_->passes_[pass_].execute = std::move(fn);
    return *this;
}

ResourceBinding RenderGraph::binding(ResourceHandle handle) const {
    const Resource& r = resources_[handle.index];
    return ResourceBinding{r.image, r.view, r.format, r.extent};
}

// ===========================================================================
// RenderGraph — compile (transient alloc + topo sort + barrier plan)
// ===========================================================================
std::expected<void, core::Error> RenderGraph::compile() {
    // 1. Materialise transient images from the pool.
    if (!resources_.empty() && pool_ == nullptr) {
        // Only an error if a transient resource actually needs the pool.
        for (const Resource& r : resources_) {
            if (!r.imported) return fail("RenderGraph::compile: transient image without a pool");
        }
    }
    for (Resource& r : resources_) {
        if (!r.imported) {
            auto img = pool_->acquire(r.format, r.extent, r.usage);
            if (!img) return std::unexpected(img.error());
            r.image = img->image;
            r.view = img->view;
        }
        r.cur_layout = r.initial_layout;
        r.last_stage = VK_PIPELINE_STAGE_2_NONE;
        r.last_access = 0;
        r.last_was_write = false;
    }

    // 2. Topological sort by resource dependencies (Kahn, smallest-index first
    //    so independent passes keep declaration order).
    const std::uint32_t n = static_cast<std::uint32_t>(passes_.size());
    std::vector<std::vector<std::uint32_t>> adj(n);
    std::vector<std::unordered_set<std::uint32_t>> edges(n);
    std::vector<int> indeg(n, 0);

    auto add_edge = [&](std::uint32_t from, std::uint32_t to) {
        if (from == to) return;
        if (edges[from].insert(to).second) {
            adj[from].push_back(to);
            ++indeg[to];
        }
    };

    struct Track {
        std::uint32_t              last_writer = UINT32_MAX;
        std::vector<std::uint32_t> readers;
    };
    std::vector<Track> track(resources_.size());

    for (std::uint32_t p = 0; p < n; ++p) {
        for (const Access& a : passes_[p].accesses) {
            Track& t = track[a.resource];
            if (a.is_write) {
                if (t.last_writer != UINT32_MAX) add_edge(t.last_writer, p);  // WAW
                for (std::uint32_t r : t.readers) add_edge(r, p);             // WAR
                t.last_writer = p;
                t.readers.clear();
            } else {
                if (t.last_writer != UINT32_MAX) add_edge(t.last_writer, p);  // RAW
                t.readers.push_back(p);
            }
        }
    }

    order_.clear();
    order_.reserve(n);
    std::vector<std::uint32_t> ready;
    for (std::uint32_t i = 0; i < n; ++i) {
        if (indeg[i] == 0) ready.push_back(i);
    }
    while (!ready.empty()) {
        auto it = std::min_element(ready.begin(), ready.end());
        const std::uint32_t node = *it;
        ready.erase(it);
        order_.push_back(node);
        for (std::uint32_t next : adj[node]) {
            if (--indeg[next] == 0) ready.push_back(next);
        }
    }
    if (order_.size() != n) {
        return fail("RenderGraph::compile: dependency cycle between passes");
    }

    // 3. Plan barriers by walking passes in execution order and diffing each
    //    accessed resource's tracked state against what the access demands.
    pass_barriers_.assign(n, {});
    final_barriers_.clear();

    for (std::uint32_t pos = 0; pos < n; ++pos) {
        Pass& pass = passes_[order_[pos]];
        std::vector<VkImageMemoryBarrier2>& bs = pass_barriers_[pos];
        for (const Access& a : pass.accesses) {
            Resource& r = resources_[a.resource];
            const UsageState s = usage_state(a.usage);
            const bool need = (r.cur_layout != s.layout) || r.last_was_write || a.is_write;
            if (need) {
                bs.push_back(make_barrier(r.image, aspect_for_format(r.format),
                                          r.cur_layout, s.layout,
                                          r.last_stage, r.last_access, s.stage, s.access));
                r.cur_layout = s.layout;
                r.last_stage = s.stage;
                r.last_access = s.access;
                r.last_was_write = a.is_write;
            } else {
                // Read-after-read in the same layout: widen visibility so the
                // producer's write reaches this stage too.
                r.last_stage |= s.stage;
                r.last_access |= s.access;
            }
        }
    }

    // 4. Trailing transitions for imported resources that must end in a specific
    //    layout (e.g. swapchain image -> PRESENT_SRC).
    for (Resource& r : resources_) {
        if (r.imported && r.final_layout != VK_IMAGE_LAYOUT_UNDEFINED
            && r.final_layout != r.cur_layout) {
            final_barriers_.push_back(make_barrier(
                r.image, aspect_for_format(r.format), r.cur_layout, r.final_layout,
                r.last_stage, r.last_access, VK_PIPELINE_STAGE_2_NONE, 0));
            r.cur_layout = r.final_layout;
        }
    }

    return {};
}

// ===========================================================================
// RenderGraph — execute
// ===========================================================================
void RenderGraph::execute(VkCommandBuffer cmd) {
    for (std::uint32_t pos = 0; pos < order_.size(); ++pos) {
        submit_barriers(cmd, pass_barriers_[pos]);
        Pass& pass = passes_[order_[pos]];
        if (pass.execute) {
            PassContext ctx(cmd, *this);
            pass.execute(ctx);
        }
    }
    submit_barriers(cmd, final_barriers_);
}

// ===========================================================================
// Self-test: clear a transient image, copy it to a host buffer, verify readback.
// The two passes are declared out of dependency order to exercise the sort.
// ===========================================================================
std::expected<void, core::Error>
run_render_graph_self_test(const Device& device, GpuAllocator& allocator,
                           TransientImagePool& pool) {
    constexpr VkExtent2D extent{16, 16};
    constexpr VkDeviceSize byte_count = static_cast<VkDeviceSize>(extent.width)
                                        * extent.height * 4;
    const VkClearColorValue clear{{0.25F, 0.50F, 0.75F, 1.0F}};
    const std::array<std::uint8_t, 4> expected{64, 128, 191, 255};

    VkCommandPool cmd_pool = VK_NULL_HANDLE;
    VkCommandPoolCreateInfo pool_info{};
    pool_info.sType = VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO;
    pool_info.flags = VK_COMMAND_POOL_CREATE_TRANSIENT_BIT;
    pool_info.queueFamilyIndex = device.queue_families().graphics;
    if (vkCreateCommandPool(device.handle(), &pool_info, nullptr, &cmd_pool) != VK_SUCCESS) {
        return fail("render graph self-test: vkCreateCommandPool failed");
    }

    std::expected<void, core::Error> result{};
    auto readback = allocator.create_buffer(
        byte_count, VK_BUFFER_USAGE_TRANSFER_DST_BIT, VMA_MEMORY_USAGE_AUTO,
        VMA_ALLOCATION_CREATE_HOST_ACCESS_RANDOM_BIT | VMA_ALLOCATION_CREATE_MAPPED_BIT);
    if (!readback) {
        result = std::unexpected(readback.error());
    } else {
        const VkBuffer readback_buffer = readback->handle();

        RenderGraph graph(pool);
        const ResourceHandle target = graph.create_transient_image(
            "selftest_target", VK_FORMAT_R8G8B8A8_UNORM, extent,
            VK_IMAGE_USAGE_TRANSFER_DST_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT);

        // Producer: clear the image. Declared first so the read below picks it
        // up as the producer of `target` (RAW edge clear -> copy).
        graph.add_pass("clear", PassType::transfer)
            .writes(target, ResourceUsage::transfer_dst)
            .execute([=](PassContext& ctx) {
                const ResourceBinding b = ctx.resolve(target);
                const VkImageSubresourceRange range{VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
                vkCmdClearColorImage(ctx.cmd(), b.image,
                                     VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, &clear, 1, &range);
            });

        // Consumer: copy the cleared image to the host buffer. The graph inserts
        // the TRANSFER_DST -> TRANSFER_SRC barrier between the two passes.
        graph.add_pass("copy_to_host", PassType::transfer)
            .reads(target, ResourceUsage::transfer_src)
            .execute([=](PassContext& ctx) {
                const ResourceBinding b = ctx.resolve(target);
                VkBufferImageCopy region{};
                region.imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
                region.imageExtent = {extent.width, extent.height, 1};
                vkCmdCopyImageToBuffer(ctx.cmd(), b.image,
                                       VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
                                       readback_buffer, 1, &region);
            });

        if (auto compiled = graph.compile(); !compiled) {
            result = std::unexpected(compiled.error());
        } else {
            VkCommandBufferAllocateInfo alloc{};
            alloc.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO;
            alloc.commandPool = cmd_pool;
            alloc.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
            alloc.commandBufferCount = 1;
            VkCommandBuffer cmd = VK_NULL_HANDLE;
            vkAllocateCommandBuffers(device.handle(), &alloc, &cmd);

            VkCommandBufferBeginInfo begin{};
            begin.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO;
            begin.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
            vkBeginCommandBuffer(cmd, &begin);
            graph.execute(cmd);
            vkEndCommandBuffer(cmd);

            VkCommandBufferSubmitInfo cmd_info{};
            cmd_info.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_SUBMIT_INFO;
            cmd_info.commandBuffer = cmd;
            VkSubmitInfo2 submit{};
            submit.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO_2;
            submit.commandBufferInfoCount = 1;
            submit.pCommandBufferInfos = &cmd_info;

            if (vkQueueSubmit2(device.graphics_queue(), 1, &submit, VK_NULL_HANDLE)
                != VK_SUCCESS) {
                result = fail("render graph self-test: vkQueueSubmit2 failed");
            } else {
                vkQueueWaitIdle(device.graphics_queue());
                std::array<std::uint8_t, 4> got{};
                std::memcpy(got.data(), readback->mapped(), got.size());
                for (std::size_t i = 0; i < got.size(); ++i) {
                    const int diff = static_cast<int>(got[i]) - static_cast<int>(expected[i]);
                    if (diff < -2 || diff > 2) {
                        result = fail(std::format(
                            "render graph self-test: readback mismatch at {} (got {}, want {})",
                            i, got[i], expected[i]));
                        break;
                    }
                }
            }
            vkFreeCommandBuffers(device.handle(), cmd_pool, 1, &cmd);
        }
    }

    pool.reset();
    vkDestroyCommandPool(device.handle(), cmd_pool, nullptr);
    return result;
}

} // namespace engine::rhi
