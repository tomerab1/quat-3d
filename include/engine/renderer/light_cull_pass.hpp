#pragma once

// LightCullPass — clustered light culling on the GPU (ROADMAP R10.1).
//
// Each frame the scene's lights (up to max_lights) and the camera's cluster
// parameters are written into this pass's host-visible buffers, then two
// graph passes run:
//   light_cull_reset (transfer) — zero the global index cursor;
//   light_cull       (compute)  — light_cull.slang bins every light's sphere
//                                 into the froxels it overlaps, writing the
//                                 per-cluster (offset, count) grid and the
//                                 compact light-index list.
// The deferred lighting pass and the forward transparent pass then read only
// their cluster's lights. All buffers are imported into the render graph, so
// the compute -> compute / fragment hazards are synchronised by the graph's
// buffer barriers.
//
// Buffers are persistent (no per-frame allocation) and owned by the pass
// instance: use one instance per frame in flight (like the other passes'
// per-slot state), and keep the graph's work finished before the next
// add_to_graph on the same instance.

#include <array>
#include <cstdint>
#include <expected>
#include <memory>
#include <span>
#include <string>

#include <glm/glm.hpp>
#include <vulkan/vulkan.h>

#include "engine/core/error.hpp"
#include "engine/renderer/light_clusters.hpp"
#include "engine/rhi/descriptor_buffer.hpp"
#include "engine/rhi/gpu_allocator.hpp"
#include "engine/rhi/managed_pipeline.hpp"
#include "engine/rhi/render_graph.hpp"

namespace engine::rhi {
class Device;
class PipelineCache;
}

namespace engine::renderer {

// Device addresses + ranges of one frame's clustered-light buffers, as bound
// by the consumer passes (see write_clustered_light_descriptors).
struct ClusteredLightBuffers {
    VkDeviceAddress params = 0;  // ClusterParams (uniform)
    VkDeviceAddress lights = 0;  // GpuLight[]
    VkDeviceAddress grid = 0;    // uint2 (offset, count) per cluster
    VkDeviceAddress indices = 0; // uint light indices
    VkDeviceSize    params_size = 0;
    VkDeviceSize    lights_size = 0;
    VkDeviceSize    grid_size = 0;
    VkDeviceSize    indices_size = 0;
};

// One frame's culled lights: the buffers plus their render-graph handles.
// Default-constructed = no clustered lights (consumers bind an empty set).
struct ClusteredLights {
    ClusteredLightBuffers buffers{};
    rhi::ResourceHandle   params{};
    rhi::ResourceHandle   lights{};
    rhi::ResourceHandle   grid{};
    rhi::ResourceHandle   indices{};
    rhi::ResourceHandle   counter{}; // [0] = indices requested this frame (diagnostics)
    std::uint32_t         light_count = 0;

    [[nodiscard]] bool valid() const { return grid.valid(); }
};

// Descriptor-layout entries for the four clustered-light bindings at
// first_binding .. first_binding + 3: params (uniform buffer), lights, grid,
// indices (storage buffers) — the order consumers declare in Slang.
[[nodiscard]] std::array<rhi::DescriptorBinding, 4>
clustered_light_bindings(std::uint32_t first_binding, VkShaderStageFlags stages);

// Writes those four descriptors.
void write_clustered_light_descriptors(rhi::DescriptorBuffer& descriptor,
                                       std::uint32_t first_binding,
                                       const ClusteredLightBuffers& buffers);

// Declares a consumer pass's reads of `lights` (no-op when invalid). `usage`
// is buffer_read_compute or buffer_read_fragment.
void read_clustered_lights(rhi::RenderGraph::PassBuilder& pass, const ClusteredLights& lights,
                           rhi::ResourceUsage usage);

// A zero-light cluster set for consumers that run without a cull pass
// (descriptors must still be valid): params with no lights + one small zeroed
// storage buffer standing in for lights / grid / indices.
class EmptyLightClusters {
public:
    [[nodiscard]] static std::expected<EmptyLightClusters, core::Error>
    create(const rhi::Device& device, rhi::GpuAllocator& allocator);

    [[nodiscard]] const ClusteredLightBuffers& buffers() const { return buffers_; }

private:
    rhi::GpuBuffer        params_;
    rhi::GpuBuffer        storage_;
    ClusteredLightBuffers buffers_{};
};

class LightCullPass {
public:
    [[nodiscard]] static std::expected<LightCullPass, core::Error>
    create(const rhi::Device& device, rhi::GpuAllocator& allocator, rhi::PipelineCache& cache,
           const std::string& cooked_shader_dir);

    LightCullPass() = default;
    LightCullPass(LightCullPass&&) noexcept = default;
    LightCullPass& operator=(LightCullPass&&) noexcept = default;
    LightCullPass(const LightCullPass&) = delete;
    LightCullPass& operator=(const LightCullPass&) = delete;

    // Uploads `lights` (the first max_lights) and the clusters of the camera
    // (`view`, perspective `proj` — unjittered is fine: shading maps samples
    // through these same matrices) and declares the reset + cull passes.
    // Fails for a non-perspective projection.
    [[nodiscard]] std::expected<ClusteredLights, core::Error>
    add_to_graph(rhi::RenderGraph& graph, const glm::mat4& view, const glm::mat4& proj,
                 std::span<const GpuLight> lights);

private:
    // Heap-held so the descriptor buffer's pointers to the layout and entry
    // points stay valid when the pass is moved.
    struct State {
        rhi::DescriptorBufferFunctions fns{};
        rhi::DescriptorSetLayout       layout;
        rhi::ManagedPipeline           pipeline;
        rhi::DescriptorBuffer          descriptor; // written once: the buffers never move
        rhi::GpuBuffer                 params;     // host-visible ClusterParams
        rhi::GpuBuffer                 lights;     // host-visible GpuLight[max_lights]
        rhi::GpuBuffer                 grid;       // device-local uint2[cluster_count]
        rhi::GpuBuffer                 indices;    // device-local uint[max_cluster_light_indices]
        rhi::GpuBuffer                 counter;    // device-local uint
        ClusteredLightBuffers          addresses{};
    };
    std::unique_ptr<State> state_;
};

} // namespace engine::renderer
