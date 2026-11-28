#pragma once

// InstanceCuller — the GPU instance cull (ROADMAP R6 / R9.4) a draw pass runs
// over its instanced draw list before drawing it; the contract, the tests
// and the CPU reference are in instance_cull.hpp, the kernels in
// instance_cull.slang.
//
// Per frame the owning pass (GBuffer, shadow casters) calls add_to_graph()
// with its draw list, the instance array it uploaded and its views. That
// declares three graph passes: a counter reset (transfer), the cull
// (compute) and the finalize (compute), which leave per view a visible list
// (instance indices, compacted into each draw's range) and per draw and view
// the indexed indirect commands plus a draw count. The pass then declares
// its reads (declare_reads), pushes visible_address(view) to its vertex
// shaders (lib/instancing.slang reads instances[visible[InstanceIndex]]) and
// records each draw with draw().
//
// Devices without drawIndirectFirstInstance (none of the engine's targets,
// but the feature is optional) keep the direct draws: the views become
// keep-all, so every visible list is the draw's full range, and draw()
// issues vkCmdDrawIndexed. Without multiDrawIndirect one indirect call is
// recorded per submesh; without drawIndirectCount empty draws are recorded
// with zero instances instead of being skipped by the GPU.
//
// Owned per frame slot by its pass, like InstanceBuffer: rewriting the
// buffers while recording a frame never races the GPU.

#include <cstdint>
#include <expected>
#include <memory>
#include <span>
#include <string>
#include <vector>

#include <glm/glm.hpp>
#include <vulkan/vulkan.h>

#include "engine/core/error.hpp"
#include "engine/renderer/draw_batching.hpp"
#include "engine/renderer/instance_cull.hpp"
#include "engine/rhi/descriptor_buffer.hpp"
#include "engine/rhi/gpu_allocator.hpp"
#include "engine/rhi/managed_pipeline.hpp"
#include "engine/rhi/render_graph.hpp"

namespace engine::rhi {
class Device;
class PipelineCache;
} // namespace engine::rhi

namespace engine::renderer {

// What one frame's cull reads besides the draw list.
struct InstanceCullInput {
    VkDeviceAddress instances = 0;       // the uploaded InstanceData array
    std::span<const CullViewGpu> views;  // at most max_cull_views
    glm::vec3 camera{0.0F};              // caster-distance origin
    // GpuCascades buffer for cascade views (graph handle + address); unset
    // when no view is a cascade.
    rhi::ResourceHandle cascades{};
    VkDeviceAddress     cascades_address = 0;
};

class InstanceCuller {
public:
    [[nodiscard]] static std::expected<InstanceCuller, core::Error>
    create(const rhi::Device& device, rhi::GpuAllocator& allocator, rhi::PipelineCache& cache,
           const std::string& cooked_shader_dir);

    InstanceCuller() = default;
    InstanceCuller(InstanceCuller&&) noexcept = default;
    InstanceCuller& operator=(InstanceCuller&&) noexcept = default;
    InstanceCuller(const InstanceCuller&) = delete;
    InstanceCuller& operator=(const InstanceCuller&) = delete;

    // Declares this frame's cull of `list` (draw i of the list is cull draw
    // i; draws without a valid mesh get no commands). The list must be the
    // one whose instances were uploaded to `in.instances`.
    [[nodiscard]] std::expected<void, core::Error>
    add_to_graph(rhi::RenderGraph& graph, const InstancedDrawList& list,
                 const InstanceCullInput& in);

    // The consuming pass's reads: visible lists (vertex) and the commands /
    // draw counts (indirect).
    void declare_reads(rhi::RenderGraph::PassBuilder& pass) const;

    // Address of view `view`'s visible list (push it to the vertex shaders).
    [[nodiscard]] VkDeviceAddress visible_address(std::uint32_t view) const;

    // Records draw `draw` of the list for view `view` (vertex / index
    // buffers bound by the caller).
    void draw(VkCommandBuffer cmd, std::uint32_t view, std::uint32_t draw) const;

    // True when draws are GPU-culled (indirect); false on the direct fallback.
    [[nodiscard]] bool indirect() const { return indirect_; }
    [[nodiscard]] std::uint32_t view_count() const { return view_count_; }

    // This frame's outputs as graph resources (invalid when nothing is
    // culled), for tests and tools that read them back: visible lists
    // (view-major, instance_count() entries per view), indirect commands
    // (view-major, command_count() per view) and draw counts (view-major,
    // one per draw of the list).
    struct Outputs {
        rhi::ResourceHandle visible;
        rhi::ResourceHandle commands;
        rhi::ResourceHandle draw_counts;
    };
    [[nodiscard]] Outputs outputs() const { return {h_visible_, h_commands_, h_draw_counts_}; }
    [[nodiscard]] std::uint32_t command_count() const { return command_count_; }
    [[nodiscard]] std::uint32_t instance_count() const { return instance_count_; }

private:
    struct Kernel {
        rhi::DescriptorSetLayout layout;
        rhi::ManagedPipeline     pipeline;
        rhi::DescriptorBuffer    set;
    };
    // Heap-held: a DescriptorBuffer points at its functions and layout, so
    // they must not move with the culler.
    struct Kernels {
        rhi::DescriptorBufferFunctions fns{};
        Kernel                         cull;
        Kernel                         finalize;
    };
    struct DrawRecord {
        std::uint32_t first_command = 0;
        std::uint32_t command_count = 0;
        std::uint32_t first_instance = 0;
        std::uint32_t instance_count = 0;
    };

    [[nodiscard]] std::expected<void, core::Error>
    ensure(rhi::GpuBuffer& buffer, VkDeviceSize bytes, VkBufferUsageFlags usage, bool host);
    [[nodiscard]] std::expected<void, core::Error> upload_inputs(const InstancedDrawList& list,
                                                                 const InstanceCullInput& in);
    void write_sets(const InstanceCullInput& in);

    const rhi::Device*             device_ = nullptr;    // non-owning
    rhi::GpuAllocator*             allocator_ = nullptr; // non-owning
    std::vector<std::uint32_t>     slots_; // reflected binding of each resource
    std::unique_ptr<Kernels>       kernels_;
    bool                           indirect_ = false;
    bool                           multi_draw_ = false;
    bool                           draw_count_ = false;

    // Host-written inputs.
    rhi::GpuBuffer params_;
    rhi::GpuBuffer draws_;
    rhi::GpuBuffer chunks_;
    rhi::GpuBuffer submeshes_;
    rhi::GpuBuffer no_cascades_; // bound when no view is a cascade
    // GPU-written outputs.
    rhi::GpuBuffer counts_;
    rhi::GpuBuffer visible_;
    rhi::GpuBuffer commands_;
    rhi::GpuBuffer draw_counts_;
    VkDeviceAddress visible_address_ = 0;

    std::vector<DrawRecord>   records_;
    std::uint32_t             view_count_ = 0;
    std::uint32_t             draw_count_total_ = 0;
    std::uint32_t             command_count_ = 0;
    std::uint32_t             instance_count_ = 0;
    std::uint32_t             chunk_count_ = 0;
    std::uint32_t             groups_x_ = 0;
    rhi::ResourceHandle       h_counts_{};
    rhi::ResourceHandle       h_visible_{};
    rhi::ResourceHandle       h_commands_{};
    rhi::ResourceHandle       h_draw_counts_{};
};

} // namespace engine::renderer
