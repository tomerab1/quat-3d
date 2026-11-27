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
    case ResourceUsage::sampled_compute:
        return {VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
                VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT,
                VK_ACCESS_2_SHADER_SAMPLED_READ_BIT};
    case ResourceUsage::sampled_graphics:
        return {VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
                VK_PIPELINE_STAGE_2_VERTEX_SHADER_BIT | VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT,
                VK_ACCESS_2_SHADER_SAMPLED_READ_BIT};
    case ResourceUsage::storage_read:
        return {VK_IMAGE_LAYOUT_GENERAL,
                VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT,
                VK_ACCESS_2_SHADER_STORAGE_READ_BIT};
    case ResourceUsage::storage_write:
        return {VK_IMAGE_LAYOUT_GENERAL,
                VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT,
                VK_ACCESS_2_SHADER_STORAGE_WRITE_BIT};
    case ResourceUsage::storage_read_write:
        return {VK_IMAGE_LAYOUT_GENERAL,
                VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT,
                VK_ACCESS_2_SHADER_STORAGE_READ_BIT | VK_ACCESS_2_SHADER_STORAGE_WRITE_BIT};
    case ResourceUsage::transfer_src:
        return {VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
                VK_PIPELINE_STAGE_2_ALL_TRANSFER_BIT, VK_ACCESS_2_TRANSFER_READ_BIT};
    case ResourceUsage::transfer_dst:
        return {VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
                VK_PIPELINE_STAGE_2_ALL_TRANSFER_BIT, VK_ACCESS_2_TRANSFER_WRITE_BIT};
    case ResourceUsage::present:
        // No access, but a real stage: the transition into PRESENT_SRC must be
        // ordered before the semaphore signal present waits on (issue #9).
        return {VK_IMAGE_LAYOUT_PRESENT_SRC_KHR, VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT, 0};
    // Buffers have no layout; UNDEFINED is never used for them.
    case ResourceUsage::buffer_read_compute:
        return {VK_IMAGE_LAYOUT_UNDEFINED, VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT,
                VK_ACCESS_2_SHADER_STORAGE_READ_BIT | VK_ACCESS_2_UNIFORM_READ_BIT};
    case ResourceUsage::buffer_write_compute:
        return {VK_IMAGE_LAYOUT_UNDEFINED, VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT,
                VK_ACCESS_2_SHADER_STORAGE_READ_BIT | VK_ACCESS_2_SHADER_STORAGE_WRITE_BIT};
    case ResourceUsage::buffer_read_fragment:
        return {VK_IMAGE_LAYOUT_UNDEFINED, VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT,
                VK_ACCESS_2_SHADER_STORAGE_READ_BIT | VK_ACCESS_2_UNIFORM_READ_BIT};
    case ResourceUsage::buffer_read_vertex:
        return {VK_IMAGE_LAYOUT_UNDEFINED, VK_PIPELINE_STAGE_2_VERTEX_SHADER_BIT,
                VK_ACCESS_2_SHADER_STORAGE_READ_BIT | VK_ACCESS_2_UNIFORM_READ_BIT};
    case ResourceUsage::buffer_read_indirect:
        return {VK_IMAGE_LAYOUT_UNDEFINED, VK_PIPELINE_STAGE_2_DRAW_INDIRECT_BIT,
                VK_ACCESS_2_INDIRECT_COMMAND_READ_BIT};
    }
    return {VK_IMAGE_LAYOUT_UNDEFINED, VK_PIPELINE_STAGE_2_NONE, 0};
}

// Which resource kinds a usage applies to (transfers apply to both).
[[nodiscard]] bool usage_fits_buffer(ResourceUsage usage) {
    switch (usage) {
    case ResourceUsage::buffer_read_compute:
    case ResourceUsage::buffer_write_compute:
    case ResourceUsage::buffer_read_fragment:
    case ResourceUsage::buffer_read_vertex:
    case ResourceUsage::buffer_read_indirect:
    case ResourceUsage::transfer_src:
    case ResourceUsage::transfer_dst:
        return true;
    default:
        return false;
    }
}

[[nodiscard]] bool usage_fits_image(ResourceUsage usage) {
    switch (usage) {
    case ResourceUsage::buffer_read_compute:
    case ResourceUsage::buffer_write_compute:
    case ResourceUsage::buffer_read_fragment:
    case ResourceUsage::buffer_read_vertex:
    case ResourceUsage::buffer_read_indirect:
        return false;
    default:
        return true;
    }
}

[[nodiscard]] VkBufferMemoryBarrier2
make_buffer_barrier(VkBuffer buffer, VkDeviceSize size, VkPipelineStageFlags2 src_stage,
                    VkAccessFlags2 src_access, VkPipelineStageFlags2 dst_stage,
                    VkAccessFlags2 dst_access) {
    VkBufferMemoryBarrier2 barrier{};
    barrier.sType = VK_STRUCTURE_TYPE_BUFFER_MEMORY_BARRIER_2;
    barrier.srcStageMask = src_stage;
    barrier.srcAccessMask = src_access;
    barrier.dstStageMask = dst_stage;
    barrier.dstAccessMask = dst_access;
    barrier.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    barrier.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    barrier.buffer = buffer;
    barrier.offset = 0;
    barrier.size = size;
    return barrier;
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

void submit_barriers(VkCommandBuffer cmd, std::span<const VkImageMemoryBarrier2> barriers,
                     std::span<const VkBufferMemoryBarrier2> buffer_barriers = {}) {
    if (barriers.empty() && buffer_barriers.empty()) return;
    VkDependencyInfo dep{};
    dep.sType = VK_STRUCTURE_TYPE_DEPENDENCY_INFO;
    dep.imageMemoryBarrierCount = static_cast<std::uint32_t>(barriers.size());
    dep.pImageMemoryBarriers = barriers.data();
    dep.bufferMemoryBarrierCount = static_cast<std::uint32_t>(buffer_barriers.size());
    dep.pBufferMemoryBarriers = buffer_barriers.data();
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
        e.idle_resets = e.in_use ? 0U : e.idle_resets + 1U;
        e.in_use = false;
    }
    // Evict long-idle images (audit A5). Safe: an entry idle for at least one
    // reset was not used by the frame that just completed.
    std::erase_if(entries_, [this](Entry& e) {
        if (e.idle_resets <= max_idle_resets) return false;
        if (e.view != VK_NULL_HANDLE && device_ != nullptr) {
            vkDestroyImageView(device_->handle(), e.view, nullptr);
        }
        return true; // GpuImage's destructor releases the allocation
    });
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
                                         VkImageLayout final_layout,
                                         VkPipelineStageFlags2 available_stage,
                                         VkAccessFlags2 available_access,
                                         VkPipelineStageFlags2 final_stage) {
    Resource r;
    r.available_stage = available_stage;
    r.available_access = available_access;
    r.final_stage = final_stage;
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

ResourceHandle RenderGraph::import_buffer(const char* name, VkBuffer buffer, VkDeviceSize size,
                                          VkPipelineStageFlags2 available_stage,
                                          VkAccessFlags2 available_access) {
    Resource r;
    r.name = name;
    r.imported = true;
    r.is_buffer = true;
    r.buffer = buffer;
    r.buffer_size = size;
    r.available_stage = available_stage;
    r.available_access = available_access;
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
    return ResourceBinding{r.image, r.view, r.format, r.extent, r.buffer, r.buffer_size};
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
        // Whatever made an imported image available acts as its last "write".
        r.write_stage = r.available_stage;
        r.write_access = r.available_access;
        r.transition_stage = VK_PIPELINE_STAGE_2_NONE;
        r.read_stages = VK_PIPELINE_STAGE_2_NONE;
        r.visible_stages = VK_PIPELINE_STAGE_2_NONE;
        r.visible_access = 0;
    }

    // A final layout needs a real second scope for its trailing transition.
    for (const Resource& r : resources_) {
        if (r.imported && !r.is_buffer && r.final_layout != VK_IMAGE_LAYOUT_UNDEFINED &&
            r.final_stage == VK_PIPELINE_STAGE_2_NONE) {
            return fail(std::format("RenderGraph::compile: image '{}' has a final layout but "
                                    "final_stage NONE (its final transition would be ordered "
                                    "before nothing)",
                                    r.name));
        }
    }

    // Every access must fit its resource kind (a layout-bearing image usage
    // on a buffer, or a buffer usage on an image, is a declaration bug).
    for (const Pass& p : passes_) {
        for (const Access& a : p.accesses) {
            if (a.resource >= resources_.size()) {
                return fail(std::format("RenderGraph::compile: pass '{}' uses an invalid handle",
                                        p.name));
            }
            const Resource& r = resources_[a.resource];
            if (r.is_buffer ? !usage_fits_buffer(a.usage) : !usage_fits_image(a.usage)) {
                return fail(std::format(
                    "RenderGraph::compile: pass '{}' uses {} '{}' with a {} usage", p.name,
                    r.is_buffer ? "buffer" : "image", r.name, r.is_buffer ? "image" : "buffer"));
            }
        }
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
    pass_buffer_barriers_.assign(n, {});
    final_barriers_.clear();

    // Buffer hazards, planned like images minus layouts: a write waits for
    // the last write (WAW) and every read since (WAR) — nothing when the
    // buffer is untouched; a read waits for the last write unless an earlier
    // barrier already made it visible to this stage/access (RAW).
    const auto plan_buffer_access = [](Resource& r, const UsageState& s, bool is_write,
                                       std::vector<VkBufferMemoryBarrier2>& out) {
        const bool has_write =
            r.write_stage != VK_PIPELINE_STAGE_2_NONE || r.write_access != 0;
        if (is_write) {
            if (has_write || r.read_stages != VK_PIPELINE_STAGE_2_NONE) {
                out.push_back(make_buffer_barrier(r.buffer, r.buffer_size,
                                                  r.write_stage | r.read_stages, r.write_access,
                                                  s.stage, s.access));
            }
            r.write_stage = s.stage;
            r.write_access = s.access;
            r.read_stages = VK_PIPELINE_STAGE_2_NONE;
            r.visible_stages = VK_PIPELINE_STAGE_2_NONE;
            r.visible_access = 0;
            return;
        }
        const bool covered =
            (s.stage & ~r.visible_stages) == 0 && (s.access & ~r.visible_access) == 0;
        if (!covered && has_write) {
            out.push_back(make_buffer_barrier(r.buffer, r.buffer_size, r.write_stage,
                                              r.write_access, s.stage, s.access));
            r.visible_stages |= s.stage;
            r.visible_access |= s.access;
        }
        r.read_stages |= s.stage;
    };

    for (std::uint32_t pos = 0; pos < n; ++pos) {
        Pass& pass = passes_[order_[pos]];
        std::vector<VkImageMemoryBarrier2>& bs = pass_barriers_[pos];
        for (const Access& a : pass.accesses) {
            Resource& r = resources_[a.resource];
            const UsageState s = usage_state(a.usage);
            if (r.is_buffer) {
                plan_buffer_access(r, s, a.is_write, pass_buffer_barriers_[pos]);
                continue;
            }
            const bool layout_change = r.cur_layout != s.layout;
            if (layout_change || a.is_write) {
                // Write-after-{write,read} or a transition: wait for the last
                // write and every read since, make the write visible to us.
                // read_stages also holds the destination stage of an earlier
                // read's transition, so this chains after that transition.
                bs.push_back(make_barrier(r.image, aspect_for_format(r.format), r.cur_layout,
                                          s.layout, r.write_stage | r.read_stages,
                                          r.write_access, s.stage, s.access));
                r.cur_layout = s.layout;
                if (a.is_write) {
                    // The write runs after this barrier's transition, so later
                    // readers chain onto the write alone.
                    r.write_stage = s.stage;
                    r.write_access = s.access;
                    r.transition_stage = VK_PIPELINE_STAGE_2_NONE;
                    r.read_stages = VK_PIPELINE_STAGE_2_NONE;
                    r.visible_stages = VK_PIPELINE_STAGE_2_NONE;
                    r.visible_access = 0;
                } else {
                    // The transition is a write that happens before s.stage
                    // (issue #4): later readers in other stages chain onto it.
                    r.transition_stage = s.stage;
                    r.read_stages = s.stage;
                    r.visible_stages = s.stage;
                    r.visible_access = s.access;
                }
            } else {
                // Read in the current layout. Earlier barriers made the last
                // write (and any later transition) visible only to the
                // stages/accesses they named; a reader outside them (e.g. a
                // fragment-shader read after a compute-only barrier) needs its
                // own dependency (audit A4). Its source scope is the last
                // write's stage plus, when a read transitioned the image since,
                // that transition's destination stage: the transition happened
                // after the write's stage, so waiting on the write alone would
                // not order this read after it (issue #4). Layout transition
                // writes are made available automatically, so the source
                // access stays the write's.
                const bool covered = (s.stage & ~r.visible_stages) == 0 &&
                                     (s.access & ~r.visible_access) == 0;
                const VkPipelineStageFlags2 src_stage = r.write_stage | r.transition_stage;
                const bool has_write = src_stage != VK_PIPELINE_STAGE_2_NONE ||
                                       r.write_access != 0;
                if (!covered && has_write) {
                    bs.push_back(make_barrier(r.image, aspect_for_format(r.format),
                                              r.cur_layout, r.cur_layout, src_stage,
                                              r.write_access, s.stage, s.access));
                    r.visible_stages |= s.stage;
                    r.visible_access |= s.access;
                }
                r.read_stages |= s.stage;
            }
        }
    }

    // 4. Trailing transitions for imported resources that must end in a specific
    //    layout (e.g. swapchain image -> PRESENT_SRC). The second scope is the
    //    stage the next consumer waits at (final_stage), never NONE: the
    //    transition is a write ordered only before that scope, so the
    //    consumer's wait (e.g. the render-finished semaphore signal) must
    //    chain onto it (issue #9). No access: the consumer's own dependency
    //    makes the image visible to it.
    for (Resource& r : resources_) {
        if (r.imported && !r.is_buffer && r.final_layout != VK_IMAGE_LAYOUT_UNDEFINED
            && r.final_layout != r.cur_layout) {
            final_barriers_.push_back(make_barrier(
                r.image, aspect_for_format(r.format), r.cur_layout, r.final_layout,
                r.write_stage | r.read_stages, r.write_access, r.final_stage, 0));
            r.cur_layout = r.final_layout;
        }
    }

    return {};
}

std::vector<VkImageMemoryBarrier2> RenderGraph::planned_barriers(std::string_view pass_name) const {
    for (std::size_t pos = 0; pos < order_.size() && pos < pass_barriers_.size(); ++pos) {
        if (passes_[order_[pos]].name == pass_name) return pass_barriers_[pos];
    }
    return {};
}

std::vector<VkBufferMemoryBarrier2>
RenderGraph::planned_buffer_barriers(std::string_view pass_name) const {
    for (std::size_t pos = 0; pos < order_.size() && pos < pass_buffer_barriers_.size(); ++pos) {
        if (passes_[order_[pos]].name == pass_name) return pass_buffer_barriers_[pos];
    }
    return {};
}


// ===========================================================================
// RenderGraph — execute
// ===========================================================================
void RenderGraph::execute(VkCommandBuffer cmd) {
    for (std::uint32_t pos = 0; pos < order_.size(); ++pos) {
        submit_barriers(cmd, pass_barriers_[pos], pass_buffer_barriers_[pos]);
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
