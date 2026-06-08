#pragma once

#include <cstdint>
#include <functional>
#include <string>
#include <vector>

#include <vulkan/vulkan.h>

#include "engine/core/error.hpp"
#include "engine/rhi/gpu_allocator.hpp"

namespace engine::rhi {

class Device;

// How a pass touches a resource. Determines the image layout the resource must
// be in for that pass, plus the pipeline-stage / access masks used to build the
// VkImageMemoryBarrier2 that synchronises it with prior accesses.
enum class ResourceUsage : std::uint8_t {
    color_attachment,
    depth_attachment,
    sampled,          // read in a fragment shader as a sampled image
    sampled_compute,  // read in a compute shader as a sampled image
    storage_read,     // read in a shader as a storage image
    storage_write,    // written in a shader as a storage image
    transfer_src,
    transfer_dst,
    present,
};

// Coarse pass category. Only affects nothing in barrier planning today (stages
// are derived from ResourceUsage) but documents intent and lets the executor
// group/annotate passes later.
enum class PassType : std::uint8_t { graphics, compute, transfer };

// Opaque handle into a RenderGraph's resource table. Returned by import_image /
// create_transient_image and handed to PassBuilder::reads / writes.
struct ResourceHandle {
    std::uint32_t index = UINT32_MAX;
    [[nodiscard]] bool valid() const { return index != UINT32_MAX; }
};

// Concrete Vulkan objects backing a graph resource, resolved at compile time.
struct ResourceBinding {
    VkImage     image  = VK_NULL_HANDLE;
    VkImageView view   = VK_NULL_HANDLE;
    VkFormat    format = VK_FORMAT_UNDEFINED;
    VkExtent2D  extent{};
};

// Pools transient (graph-owned) images keyed by (format, extent, usage) so the
// per-frame graph reuses GPU allocations instead of reallocating every frame.
// The graph borrows images for the duration of a frame; call reset() once the
// frame that used them has finished on the GPU to make them reusable again.
class TransientImagePool {
public:
    TransientImagePool() = default;
    TransientImagePool(const Device& device, GpuAllocator& allocator);
    ~TransientImagePool();

    TransientImagePool(TransientImagePool&& other) noexcept;
    TransientImagePool& operator=(TransientImagePool&& other) noexcept;
    TransientImagePool(const TransientImagePool&) = delete;
    TransientImagePool& operator=(const TransientImagePool&) = delete;

    // Acquire an image matching the description, reusing a free cached one when
    // possible. The returned view/image are owned by the pool — do not destroy.
    [[nodiscard]] std::expected<ResourceBinding, core::Error>
    acquire(VkFormat format, VkExtent2D extent, VkImageUsageFlags usage);

    // Mark every acquired image free for reuse on the next frame.
    void reset();

private:
    void destroy() noexcept;

    struct Entry {
        GpuImage          image;
        VkImageView       view   = VK_NULL_HANDLE;
        VkFormat          format = VK_FORMAT_UNDEFINED;
        VkExtent2D        extent{};
        VkImageUsageFlags usage  = 0;
        bool              in_use = false;
    };

    const Device*      device_    = nullptr;   // non-owning
    GpuAllocator*      allocator_ = nullptr;   // non-owning
    std::vector<Entry> entries_;
};

class RenderGraph;

// Handed to each pass's execute lambda: exposes the command buffer being
// recorded and resolves resource handles to their concrete Vulkan objects.
class PassContext {
public:
    PassContext(VkCommandBuffer cmd, const RenderGraph& graph)
        : cmd_(cmd), graph_(&graph) {}

    [[nodiscard]] VkCommandBuffer cmd() const { return cmd_; }
    [[nodiscard]] ResourceBinding resolve(ResourceHandle handle) const;

private:
    VkCommandBuffer    cmd_;
    const RenderGraph* graph_;
};

using PassExecuteFn = std::function<void(PassContext&)>;

// A per-frame, declarative render graph. Callers import external images (e.g.
// the swapchain image) and/or declare transient images, register passes that
// read/write those resources, then compile() (topological sort + barrier plan)
// and execute(cmd). Rebuilt from scratch each frame — graph compilation is
// cheap and avoids stale-state bugs. Used locally; non-copyable, non-movable.
class RenderGraph {
public:
    RenderGraph() = default;
    explicit RenderGraph(TransientImagePool& pool) : pool_(&pool) {}

    RenderGraph(const RenderGraph&) = delete;
    RenderGraph& operator=(const RenderGraph&) = delete;

    // Import an externally-owned image. `current_layout` is the layout the image
    // is in when the graph begins; `final_layout` is the layout the graph leaves
    // it in (a trailing barrier is inserted if the last pass left it elsewhere).
    ResourceHandle import_image(const char* name, VkImage image, VkImageView view,
                                VkFormat format, VkExtent2D extent,
                                VkImageLayout current_layout,
                                VkImageLayout final_layout);

    // Declare a graph-managed transient image. Backed by the transient pool at
    // compile time. `usage` must cover every way passes touch it.
    ResourceHandle create_transient_image(const char* name, VkFormat format,
                                          VkExtent2D extent, VkImageUsageFlags usage);

    // Fluent pass declaration. Returned by reference into the graph's pass list;
    // valid until the next add_pass / compile call.
    class PassBuilder {
    public:
        PassBuilder(RenderGraph& graph, std::uint32_t pass) : graph_(&graph), pass_(pass) {}
        PassBuilder& reads(ResourceHandle handle, ResourceUsage usage);
        PassBuilder& writes(ResourceHandle handle, ResourceUsage usage);
        PassBuilder& execute(PassExecuteFn fn);

    private:
        RenderGraph*  graph_;
        std::uint32_t pass_;
    };

    [[nodiscard]] PassBuilder add_pass(const char* name, PassType type);

    // Allocate transient images, topologically order passes by their resource
    // dependencies, and plan every barrier. Errors on a dependency cycle or a
    // failed transient allocation.
    [[nodiscard]] std::expected<void, core::Error> compile();

    // Replay the compiled plan into `cmd`: barriers then execute lambda, per
    // pass in dependency order, followed by trailing import final-layout barriers.
    void execute(VkCommandBuffer cmd);

    [[nodiscard]] ResourceBinding binding(ResourceHandle handle) const;

private:
    struct Access {
        std::uint32_t resource = UINT32_MAX;
        ResourceUsage usage{};
        bool          is_write = false;
    };

    struct Pass {
        std::string         name;
        PassType            type{};
        std::vector<Access> accesses;
        PassExecuteFn       execute;
    };

    struct Resource {
        std::string       name;
        bool              imported = false;
        VkImage           image  = VK_NULL_HANDLE;
        VkImageView       view   = VK_NULL_HANDLE;
        VkFormat          format = VK_FORMAT_UNDEFINED;
        VkExtent2D        extent{};
        VkImageUsageFlags usage  = 0;                 // transient only
        VkImageLayout     initial_layout = VK_IMAGE_LAYOUT_UNDEFINED;
        VkImageLayout     final_layout   = VK_IMAGE_LAYOUT_UNDEFINED;
        // Tracked while planning barriers:
        VkImageLayout         cur_layout = VK_IMAGE_LAYOUT_UNDEFINED;
        VkPipelineStageFlags2 last_stage = VK_PIPELINE_STAGE_2_NONE;
        VkAccessFlags2        last_access = 0;
        bool                  last_was_write = false;
    };

    TransientImagePool* pool_ = nullptr;   // non-owning

    std::vector<Resource> resources_;
    std::vector<Pass>     passes_;

    // Filled by compile():
    std::vector<std::uint32_t>                      order_;          // pass indices
    std::vector<std::vector<VkImageMemoryBarrier2>> pass_barriers_;  // per order_ slot
    std::vector<VkImageMemoryBarrier2>              final_barriers_;
};

// Builds a tiny two-pass graph (clear a transient image, then copy it to a host
// buffer) with the passes declared out of dependency order, compiles + executes
// it on a one-time command buffer, and verifies the readback matches the clear
// colour. Exercises the transient pool, topological sort, and barrier insertion.
[[nodiscard]] std::expected<void, core::Error>
run_render_graph_self_test(const Device& device, GpuAllocator& allocator,
                           TransientImagePool& pool);

} // namespace engine::rhi
