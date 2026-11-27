#pragma once

#include <cstdint>
#include <functional>
#include <string>
#include <string_view>
#include <vector>

#include <vulkan/vulkan.h>

#include "engine/core/error.hpp"
#include "engine/rhi/gpu_allocator.hpp"

namespace engine::rhi {

class Device;

// How a pass touches a resource. Determines the image layout the resource must
// be in for that pass, plus the pipeline-stage / access masks used to build the
// VkImageMemoryBarrier2 / VkBufferMemoryBarrier2 that synchronises it with prior
// accesses. Image resources take the image usages (and transfer_src/dst);
// buffer resources take the buffer_* usages (and transfer_src/dst) — compile()
// rejects any other pairing.
enum class ResourceUsage : std::uint8_t {
    color_attachment,
    depth_attachment,
    sampled,          // read in a fragment shader as a sampled image
    sampled_compute,  // read in a compute shader as a sampled image
    sampled_graphics, // read in vertex and fragment shaders as a sampled image (e.g. VTF)
    storage_read,     // read in a shader as a storage image
    storage_write,    // written in a shader as a storage image
    storage_read_write, // read-modify-write of a storage image (e.g. bloom composite)
    transfer_src,     // image or buffer copy source
    transfer_dst,     // image or buffer copy/fill destination
    present,
    buffer_read_compute,   // storage/uniform buffer read in a compute shader
    buffer_write_compute,  // storage buffer written (incl. atomics / read-back) in compute
    buffer_read_fragment,  // storage/uniform buffer read in a fragment shader
    buffer_read_vertex,    // storage/uniform buffer read in a vertex shader
    buffer_read_indirect,  // indirect draw/dispatch arguments (vkCmdDraw*Indirect)
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
// Image resources fill image/view/format/extent; buffer resources fill
// buffer/size.
struct ResourceBinding {
    VkImage      image  = VK_NULL_HANDLE;
    VkImageView  view   = VK_NULL_HANDLE;
    VkFormat     format = VK_FORMAT_UNDEFINED;
    VkExtent2D   extent{};
    VkBuffer     buffer = VK_NULL_HANDLE;
    VkDeviceSize size   = 0;
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

    // Mark every acquired image free for reuse on the next frame, and free
    // images that no frame has acquired for `max_idle_resets` resets (a
    // resize or viewport drag leaves a whole set of images at the old extent
    // behind; without eviction they would accumulate for the program's life).
    // Call it only once the GPU has finished the commands that used this
    // pool's images (the frame loop calls it after the slot's fence wait):
    // an image not acquired in the previous frame is then referenced by no
    // pending work and is safe to destroy.
    void reset();

    static constexpr std::uint32_t max_idle_resets = 4;

    // Images currently owned by the pool (in use or cached).
    [[nodiscard]] std::size_t size() const { return entries_.size(); }

private:
    void destroy() noexcept;

    struct Entry {
        GpuImage          image;
        VkImageView       view   = VK_NULL_HANDLE;
        VkFormat          format = VK_FORMAT_UNDEFINED;
        VkExtent2D        extent{};
        VkImageUsageFlags usage  = 0;
        bool              in_use = false;
        std::uint32_t     idle_resets = 0; // consecutive resets without being acquired
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
    // `available_stage` / `available_access` describe what made the image
    // available to this graph — the first barrier on it waits for them. For a
    // swapchain image pass the stage the acquire semaphore is waited at, so
    // the first write (layout transition, copy, …) is chained after the
    // presentation engine releases the image. NONE means "nothing to wait for".
    //
    // `final_stage` is the mirror image: the stage at which whatever consumes
    // the image after the graph waits for it — the stage of the semaphore
    // signal that present waits on, or the source scope of the next
    // command's barrier. It becomes the second synchronization scope of the
    // trailing final-layout barrier. A layout transition is a write that is
    // ordered only before its barrier's second scope, so that scope must be a
    // real stage the consumer's wait chains onto: with NONE (as before issue
    // #9) a semaphore signal does not wait for the transition and present
    // races it (SYNC-HAZARD-PRESENT-AFTER-WRITE). The default, ALL_COMMANDS,
    // chains onto any consumer; pass a narrower stage (e.g. the frame's
    // render-finished signal stage for a swapchain image) only together with
    // a consumer that waits at that stage. NONE is rejected by compile() when
    // a final layout is requested.
    ResourceHandle import_image(const char* name, VkImage image, VkImageView view,
                                VkFormat format, VkExtent2D extent,
                                VkImageLayout current_layout,
                                VkImageLayout final_layout,
                                VkPipelineStageFlags2 available_stage = VK_PIPELINE_STAGE_2_NONE,
                                VkAccessFlags2 available_access = 0,
                                VkPipelineStageFlags2 final_stage =
                                    VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT);

    // Import an externally-owned buffer (whole range). Buffers have no layout;
    // the graph orders passes by their buffer reads/writes and plans
    // VkBufferMemoryBarrier2s for the hazards between them (RAW, WAR, WAW).
    // `available_stage` / `available_access` work as for import_image: what
    // the first access must wait for (NONE when the buffer is idle — e.g. a
    // per-frame-slot buffer whose previous use was fence-waited, or one only
    // written by the host before submission, which vkQueueSubmit already makes
    // visible).
    ResourceHandle import_buffer(const char* name, VkBuffer buffer, VkDeviceSize size,
                                 VkPipelineStageFlags2 available_stage = VK_PIPELINE_STAGE_2_NONE,
                                 VkAccessFlags2 available_access = 0);

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
    // dependencies, and plan every barrier. Errors on a dependency cycle, a
    // failed transient allocation, or a usage that does not fit the resource
    // kind (an image usage on a buffer or vice versa).
    [[nodiscard]] std::expected<void, core::Error> compile();

    // Barriers compile() planned to run before the pass named `pass_name`
    // (empty if none or unknown). For tests and debugging tools.
    [[nodiscard]] std::vector<VkImageMemoryBarrier2> planned_barriers(std::string_view pass_name) const;
    // Buffer barriers compile() planned to run before `pass_name`.
    [[nodiscard]] std::vector<VkBufferMemoryBarrier2>
    planned_buffer_barriers(std::string_view pass_name) const;
    // Trailing final-layout barriers compile() planned after the last pass.
    [[nodiscard]] const std::vector<VkImageMemoryBarrier2>& planned_final_barriers() const {
        return final_barriers_;
    }

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
        bool              is_buffer = false;              // imported buffer (no layout)
        VkBuffer          buffer = VK_NULL_HANDLE;
        VkDeviceSize      buffer_size = 0;
        VkImage           image  = VK_NULL_HANDLE;
        VkImageView       view   = VK_NULL_HANDLE;
        VkFormat          format = VK_FORMAT_UNDEFINED;
        VkExtent2D        extent{};
        VkImageUsageFlags usage  = 0;                 // transient only
        VkImageLayout     initial_layout = VK_IMAGE_LAYOUT_UNDEFINED;
        VkImageLayout     final_layout   = VK_IMAGE_LAYOUT_UNDEFINED;
        VkPipelineStageFlags2 available_stage = VK_PIPELINE_STAGE_2_NONE; // imported only
        VkAccessFlags2        available_access = 0;
        VkPipelineStageFlags2 final_stage = VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT; // imported only
        // Tracked while planning barriers. The last write (its stage/access)
        // is what every later reader must see; `visible_*` record which stages
        // and accesses a barrier already made that write visible to, and
        // `read_stages` which stages read it since (a later write or layout
        // change must wait for them: write-after-read). `transition_stage` is
        // the destination stage of the barrier whose layout transition (done
        // for a read, since the last write) the image is in: the transition is
        // itself a write, so a later reader outside that stage must chain onto
        // it, not only onto the last write (issue #4).
        VkImageLayout         cur_layout = VK_IMAGE_LAYOUT_UNDEFINED;
        VkPipelineStageFlags2 write_stage = VK_PIPELINE_STAGE_2_NONE;
        VkAccessFlags2        write_access = 0;
        VkPipelineStageFlags2 transition_stage = VK_PIPELINE_STAGE_2_NONE;
        VkPipelineStageFlags2 read_stages = VK_PIPELINE_STAGE_2_NONE;
        VkPipelineStageFlags2 visible_stages = VK_PIPELINE_STAGE_2_NONE;
        VkAccessFlags2        visible_access = 0;
    };

    TransientImagePool* pool_ = nullptr;   // non-owning

    std::vector<Resource> resources_;
    std::vector<Pass>     passes_;

    // Filled by compile():
    std::vector<std::uint32_t>                      order_;          // pass indices
    std::vector<std::vector<VkImageMemoryBarrier2>> pass_barriers_;  // per order_ slot
    std::vector<std::vector<VkBufferMemoryBarrier2>> pass_buffer_barriers_; // per order_ slot
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
